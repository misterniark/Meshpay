/**
 * @file espnow.c
 * @brief Logique du protocole ESP-NOW : découverte et paiement.
 *
 * Deux protocoles implémentés :
 *
 * 1. Découverte :
 *    CMD_START_DISCOVER → broadcast DISCOVER → collecte ANNOUNCE → événements PEER_DISCOVERED
 *
 * 2. Paiement :
 *    CMD_SEND_TX → unicast TX_LOCKED → attente ACK → événement ACK_RECEIVED ou TX_TIMEOUT
 *
 * Le callback RX est enregistré via la vtable espnow_hal_t.
 * Les données reçues sont traitées par espnow_handle_rx() qui
 * décode le type et poste l'événement correspondant dans evt_queue.
 */

#include "comm/espnow.h"
#include "comm/nonce_cache.h"
#include "comm/comm_msg.h"
#include "crypto/crypto_sign.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include <string.h>

/* ================================================================
 * Rate-limiting reception ESP-NOW [H7]
 * ================================================================ */

/** [I5] Nombre maximal de messages acceptes PAR SOURCE dans la fenetre */
#define RX_RATE_LIMIT_PER_MAC    10
/** [I5] Plafond global (anti-DoS total, somme de toutes les sources) */
#define RX_RATE_LIMIT_GLOBAL     50
/** Duree de la fenetre de rate-limiting en millisecondes */
#define RX_RATE_LIMIT_WINDOW_MS  1000
/** Nombre de MAC sources traquees simultanement (eviction LRU) */
#define RX_RATE_TABLE_SIZE       8

/**
 * [I5-fix] Rate-limiting par MAC source.
 *
 * Avant : compteur global unique — un seul pair bruyant pouvait faire
 * dropper les messages de tous les autres pairs (DoS par un seul
 * voisin malveillant).
 *
 * Maintenant : chaque MAC source a son propre compteur dans une fenetre
 * glissante. Un pair flooder ne bloque que ses propres messages, pas
 * ceux des autres. Un plafond global additionnel reste en place pour
 * proteger en cas de flood reparti sur plusieurs sources.
 *
 * La table est une LRU simple de taille fixe : quand elle est pleine
 * et qu'une nouvelle MAC arrive, l'entree la plus ancienne est evincee.
 * Taille 8 suffit pour un reseau local typique (~10 pairs a portee).
 */
typedef struct {
    uint8_t  mac[6];            /**< MAC source (zeros si slot libre) */
    uint32_t window_start_ms;   /**< Debut de la fenetre courante */
    uint8_t  count;             /**< Messages recus dans la fenetre */
    uint32_t last_seen_ms;      /**< Pour l'eviction LRU */
    bool     used;              /**< Slot occupe ? */
} rx_rate_entry_t;

static rx_rate_entry_t s_rx_rate_table[RX_RATE_TABLE_SIZE];
static portMUX_TYPE    s_rx_rate_mux = portMUX_INITIALIZER_UNLOCKED;

/**
 * Verifie et met a jour le rate-limit pour une MAC source.
 *
 * @param mac     MAC source
 * @param now_ms  Timestamp courant (ms)
 * @return true si le paquet doit etre accepte, false s'il doit etre rejete
 */
static bool rx_rate_check_and_update(const uint8_t *mac, uint32_t now_ms)
{
    bool accept = true;
    taskENTER_CRITICAL(&s_rx_rate_mux);

    /* Chercher une entree existante pour cette MAC */
    int idx = -1;
    int oldest_idx = 0;
    uint32_t oldest_ts = UINT32_MAX;
    int free_idx = -1;

    for (int i = 0; i < RX_RATE_TABLE_SIZE; i++) {
        if (!s_rx_rate_table[i].used) {
            if (free_idx < 0) free_idx = i;
            continue;
        }
        if (memcmp(s_rx_rate_table[i].mac, mac, 6) == 0) {
            idx = i;
            break;
        }
        if (s_rx_rate_table[i].last_seen_ms < oldest_ts) {
            oldest_ts  = s_rx_rate_table[i].last_seen_ms;
            oldest_idx = i;
        }
    }

    if (idx < 0) {
        /* Nouvelle MAC — allouer un slot (prefer free, fallback eviction LRU) */
        idx = (free_idx >= 0) ? free_idx : oldest_idx;
        memcpy(s_rx_rate_table[idx].mac, mac, 6);
        s_rx_rate_table[idx].window_start_ms = now_ms;
        s_rx_rate_table[idx].count = 0;
        s_rx_rate_table[idx].used = true;
    }

    /* Reset compteur si fenetre expiree */
    if ((now_ms - s_rx_rate_table[idx].window_start_ms) >= RX_RATE_LIMIT_WINDOW_MS) {
        s_rx_rate_table[idx].window_start_ms = now_ms;
        s_rx_rate_table[idx].count = 0;
    }

    s_rx_rate_table[idx].count++;
    s_rx_rate_table[idx].last_seen_ms = now_ms;

    if (s_rx_rate_table[idx].count > RX_RATE_LIMIT_PER_MAC) {
        accept = false;
    }

    taskEXIT_CRITICAL(&s_rx_rate_mux);
    return accept;
}

/* ================================================================
 * Table des TX en attente d'ACK [Lot B item 3]
 *
 * Quand le core_task demande l'envoi d'une TX_LOCKED via COMM_CMD_SEND_TX,
 * on enregistre ici (tx_id, expected_signer = tx.to) avec un deadline.
 * A la reception d'un COMM_MSG_TX_ACK, on verifie AVANT de propager
 * l'evenement que :
 *   1. le tx_id correspond a une TX qu'on a effectivement envoyee,
 *   2. ack.sender_key egale le destinataire attendu (tx.to).
 *
 * Si l'une des deux verifications echoue, l'ACK est silencieusement
 * rejete dans la couche comm. core_task ne le voit jamais.
 *
 * Cette protection est une DEFENSE EN PROFONDEUR : core_task continue
 * de verifier l'ACK contre sa lock_table. Mais filtrer au plus tot
 * reduit la surface d'attaque (un attaquant LoRa/RF qui forge un ACK
 * avec une signature valide mais une mauvaise paire sender_key/tx_id
 * ne genere meme pas d'evenement vers le core).
 *
 * Taille : 1 entree (contrainte par la marge DRAM disponible — des
 * tables plus larges faisaient deborder dram0_seg). Suffit pour le
 * cas standard ou un device n'envoie qu'un paiement a la fois.
 *
 * Cas degrade : si un auto-forward est declenche pendant qu'un paiement
 * manuel est en attente d'ACK, le 1er sera ecrase silencieusement.
 * L'utilisateur le voit cote UI en timeout au bout de
 * ESPNOW_ACK_TIMEOUT_MS (30 s). Ce n'est pas une faille de securite
 * — juste une perte d'UX. Si tu vois ce cas en production, augmente
 * la taille apres avoir libere de la RAM ailleurs (~72 octets par
 * entree supplementaire).
 *
 * Eviction par expiration (deadline depasse) ; si la table est saturee
 * AVANT expiration on ecrase l'entree la plus ancienne.
 * ================================================================ */

#define PENDING_TX_TABLE_SIZE 1
/** Aligne sur le timeout d'attente d'ACK pour rester coherent. */
#define PENDING_TX_TIMEOUT_MS ESPNOW_ACK_TIMEOUT_MS

typedef struct {
    hash_t       tx_id;             /**< Identifiant de la TX envoyee */
    public_key_t expected_signer;   /**< Cle attendue dans l'ACK (= tx.to) */
    uint32_t     deadline_ms;       /**< Apres cette date, entree expiree */
    bool         used;              /**< Slot occupe ? */
} pending_tx_entry_t;

static pending_tx_entry_t s_pending_tx_table[PENDING_TX_TABLE_SIZE];
static portMUX_TYPE       s_pending_tx_mux = portMUX_INITIALIZER_UNLOCKED;

/**
 * Marque comme libres toutes les entrees dont le deadline est depasse.
 * Doit etre appele sous s_pending_tx_mux.
 */
static void pending_tx_sweep_expired_locked(uint32_t now_ms)
{
    for (int i = 0; i < PENDING_TX_TABLE_SIZE; i++) {
        if (s_pending_tx_table[i].used &&
            (int32_t)(now_ms - s_pending_tx_table[i].deadline_ms) >= 0) {
            s_pending_tx_table[i].used = false;
        }
    }
}

/**
 * Enregistre une TX envoyee. Si la table est pleine et qu'aucune entree
 * n'a expire, on ecrase silencieusement l'entree la plus ancienne :
 * priviligier l'ACK le plus recent est correct pour notre cas (le
 * device n'envoie pas plus d'une TX par paiement utilisateur).
 */
static void pending_tx_register(const hash_t *tx_id,
                                 const public_key_t *expected_signer,
                                 uint32_t now_ms)
{
    taskENTER_CRITICAL(&s_pending_tx_mux);
    pending_tx_sweep_expired_locked(now_ms);

    /* Chercher un slot libre ; sinon, ecraser l'entree la plus ancienne. */
    int target = -1;
    uint32_t oldest_deadline = UINT32_MAX;
    int oldest_idx = 0;
    for (int i = 0; i < PENDING_TX_TABLE_SIZE; i++) {
        if (!s_pending_tx_table[i].used) {
            target = i;
            break;
        }
        if (s_pending_tx_table[i].deadline_ms < oldest_deadline) {
            oldest_deadline = s_pending_tx_table[i].deadline_ms;
            oldest_idx = i;
        }
    }
    if (target < 0) target = oldest_idx;

    s_pending_tx_table[target].tx_id           = *tx_id;
    s_pending_tx_table[target].expected_signer = *expected_signer;
    s_pending_tx_table[target].deadline_ms     = now_ms + PENDING_TX_TIMEOUT_MS;
    s_pending_tx_table[target].used            = true;

    taskEXIT_CRITICAL(&s_pending_tx_mux);
}

/**
 * Verifie qu'un ACK est attendu et signe par la bonne cle.
 *
 * Si la verification reussit, l'entree est consommee (used=false)
 * pour empecher un re-emit d'ACK valide d'etre accepte plusieurs fois.
 *
 * @return true si l'ACK est legitime, false sinon (tx_id inconnu, expire,
 *         ou cle du signataire ne correspond pas a tx.to attendu).
 */
static bool pending_tx_consume_and_verify(const hash_t *tx_id,
                                           const public_key_t *sender_key,
                                           uint32_t now_ms)
{
    bool ok = false;
    taskENTER_CRITICAL(&s_pending_tx_mux);
    pending_tx_sweep_expired_locked(now_ms);

    for (int i = 0; i < PENDING_TX_TABLE_SIZE; i++) {
        if (!s_pending_tx_table[i].used) continue;
        if (!hash_equal(&s_pending_tx_table[i].tx_id, tx_id)) continue;

        /* tx_id trouve : verifier que le signataire est bien tx.to. */
        if (public_key_equal(&s_pending_tx_table[i].expected_signer,
                              sender_key)) {
            s_pending_tx_table[i].used = false; /* consomme */
            ok = true;
        }
        /* On a trouve le tx_id : on s'arrete meme si la cle ne match pas
           (un autre slot avec le meme tx_id ne devrait pas exister). */
        break;
    }

    taskEXIT_CRITICAL(&s_pending_tx_mux);
    return ok;
}

/* ================================================================
 * Cache circulaire de nonces anti-rejeu [C8]
 *
 * Stocke les derniers nonces vus pour détecter les messages rejoués.
 * Un buffer circulaire suffit car les nonces sont aléatoires et
 * la probabilité de collision sur 32 bits est négligeable.
 * ================================================================ */

/** Instance du cache circulaire de nonces anti-rejeu */
static nonce_cache_t s_nonce_cache;

/**
 * Spinlock protégeant le cache de nonces anti-rejeu.
 * Utilisation d'un portMUX (spinlock ISR-safe) car le callback de réception
 * ESP-NOW peut être appelé depuis un contexte à priorité élevée.
 */
static portMUX_TYPE s_nonce_mux = portMUX_INITIALIZER_UNLOCKED;

/*
 * [F-EN-004] Cache anti-rejeu spécifique aux TX_LOCKED.
 *
 * Le format wire d'une TX_LOCKED ne contient pas de nonce dédié :
 * la transaction porte sa propre id (hash 32 octets). On stocke les
 * 4 premiers octets de tx.id comme "fingerprint" pour réutiliser
 * l'infra nonce_cache existante (uint32_t). Probabilité de collision
 * accidentelle : 1/2³² ≈ 2.3×10⁻¹⁰ — négligeable pour le nombre de
 * TX échangées en un cycle (qq centaines max). Un attaquant qui
 * voudrait forger une TX avec une fingerprint coïncidant avec une
 * TX légitime devrait casser SHA-256 → équivalent à forger la TX
 * elle-même, qui sera de toute façon rejetée par la validation de
 * signature dans core_task. La cache absorbe donc les rejeux des
 * captures réseau sans introduire de nouvelle surface d'attaque.
 *
 * Fenêtre effective : ~2.6 s à 50 msg/s (NONCE_CACHE_SIZE = 128).
 * Au-delà, la pending_tx_table côté émetteur et la déduplication
 * par tx_id dans le DAG (core_task) restent les filets de sécurité.
 */
static nonce_cache_t s_tx_locked_cache;
static portMUX_TYPE  s_tx_locked_mux = portMUX_INITIALIZER_UNLOCKED;

/**
 * Helper interne : extraire la fingerprint 32-bit d'un tx_id.
 * Lit les 4 premiers octets en big-endian (cohérent avec write_u32_be).
 */
static inline uint32_t tx_id_fingerprint(const hash_t *tx_id)
{
    return ((uint32_t)tx_id->bytes[0] << 24) |
           ((uint32_t)tx_id->bytes[1] << 16) |
           ((uint32_t)tx_id->bytes[2] <<  8) |
           ((uint32_t)tx_id->bytes[3]);
}

static const char *TAG = "espnow";

/* ================================================================
 * Callback de réception
 * ================================================================ */

/**
 * Callback interne appelé par la HAL quand un paquet ESP-NOW arrive.
 *
 * On décode le message et on poste l'événement dans evt_queue.
 * Cette fonction est appelée dans le contexte de la tâche Wi-Fi
 * sur ESP32, donc elle doit être rapide et non bloquante.
 *
 * Le user_ctx pointe vers la config espnow_config_t.
 */
static void rx_callback(const uint8_t *src_mac, const uint8_t *data,
                         size_t len, void *user_ctx)
{
    espnow_config_t *config = (espnow_config_t *)user_ctx;
    if (!config || !data || len == 0) return;

    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);

    /*
     * Rate-limiting [H7 / I5] a deux niveaux :
     * 1. Plafond global : protege contre un flood reparti sur plusieurs
     *    sources (ex. armee de devices compromis).
     * 2. Plafond par MAC : empeche un pair bruyant seul de bloquer les
     *    messages des autres pairs legitimes.
     *
     * Le check par-MAC est independant du global — les deux doivent
     * passer pour que le paquet soit accepte.
     *
     * [F-EN-002] Les compteurs globaux (s_rx_window_start, s_rx_count)
     * sont accedes sous le meme spinlock s_rx_rate_mux que le check
     * par-MAC dans rx_rate_check_and_update. Avant ce fix, les
     * variables locales static n'etaient pas protegees : sur ESP32
     * dual-core, deux frames recues simultanement pouvaient (a)
     * perdre un incrément de s_rx_count, (b) reset la fenetre a mi-
     * chemin, ou (c) déborder le plafond global sans déclencher le
     * frein. Le déplacement en variables de module + ENTER/EXIT
     * autour de la lecture-modification ferme cette race.
     */
    static uint32_t s_rx_window_start = 0;
    static uint8_t  s_rx_count = 0;

    bool global_overflow = false;
    taskENTER_CRITICAL(&s_rx_rate_mux);
    if ((now_ms - s_rx_window_start) >= RX_RATE_LIMIT_WINDOW_MS) {
        s_rx_window_start = now_ms;
        s_rx_count = 0;
    }
    if (s_rx_count < UINT8_MAX) {
        s_rx_count++;
    }
    if (s_rx_count > RX_RATE_LIMIT_GLOBAL) {
        global_overflow = true;
    }
    uint8_t snapshot_count = s_rx_count;
    taskEXIT_CRITICAL(&s_rx_rate_mux);

    if (global_overflow) {
        ESP_LOGW(TAG, "Rate-limit ESP-NOW global : %u msg/s (plafond=%d)",
                 snapshot_count, RX_RATE_LIMIT_GLOBAL);
        return;
    }

    if (!rx_rate_check_and_update(src_mac, now_ms)) {
        ESP_LOGW(TAG, "Rate-limit ESP-NOW par-MAC : source %02x:%02x:%02x:%02x:%02x:%02x trop bruyante (>%d msg/s)",
                 src_mac[0], src_mac[1], src_mac[2],
                 src_mac[3], src_mac[4], src_mac[5],
                 RX_RATE_LIMIT_PER_MAC);
        return;
    }

    espnow_handle_rx(config, src_mac, data, len);
}

/* ================================================================
 * Traitement des messages reçus
 * ================================================================ */

void espnow_handle_rx(const espnow_config_t *config,
                       const uint8_t *src_mac,
                       const uint8_t *data, size_t len)
{
    comm_msg_type_t type;
    if (comm_msg_get_type(data, len, &type) != 0) {
        ESP_LOGW(TAG, "Message reçu avec type inconnu");
        return;
    }

    comm_event_t evt;
    memset(&evt, 0, sizeof(evt));

    switch (type) {

    case COMM_MSG_ANNOUNCE: {
        /*
         * Un peer répond à notre DISCOVER avec sa clé publique, sa signature
         * et son alias. On vérifie la signature et le nonce anti-rejeu avant
         * de poster l'événement PEER_DISCOVERED.
         */
        comm_msg_announce_t announce;
        if (comm_msg_unpack_announce(data, len, &announce) != 0) {
            ESP_LOGW(TAG, "ANNOUNCE malformé");
            return;
        }

        /* Vérification anti-rejeu sous section critique (spinlock ISR-safe).
         * On vérifie d'abord si le nonce est déjà vu, puis on relâche le
         * verrou pour la vérification crypto (coûteuse), et on re-vérifie
         * avant d'ajouter le nonce au cache (double-check pattern). */
        taskENTER_CRITICAL(&s_nonce_mux);
        if (nonce_cache_seen(&s_nonce_cache, announce.nonce)) {
            taskEXIT_CRITICAL(&s_nonce_mux);
            ESP_LOGW(TAG, "ANNOUNCE rejete : nonce deja vu (rejeu)");
            return;
        }
        taskEXIT_CRITICAL(&s_nonce_mux);

        /* Construire le buffer signé : [nonce:4][alias_len:1][alias:N] */
        uint8_t sign_buf[4 + 1 + COMM_MSG_ALIAS_MAX];
        size_t sign_len = 4 + 1 + announce.alias_len;
        sign_buf[0] = (uint8_t)(announce.nonce >> 24);
        sign_buf[1] = (uint8_t)(announce.nonce >> 16);
        sign_buf[2] = (uint8_t)(announce.nonce >> 8);
        sign_buf[3] = (uint8_t)(announce.nonce);
        sign_buf[4] = announce.alias_len;
        memcpy(&sign_buf[5], announce.alias, announce.alias_len);

        /* Vérifier la signature Ed25519 avec la clé publique de l'émetteur
         * (opération coûteuse, effectuée hors section critique) */
        if (crypto_verify(sign_buf, sign_len,
                          &announce.device_key, &announce.signature) != ESP_OK) {
            ESP_LOGW(TAG, "ANNOUNCE rejete : signature invalide");
            return;
        }

        /* Double-check sous section critique : un autre thread a pu ajouter
         * ce nonce pendant la vérification crypto. On re-vérifie puis on ajoute. */
        taskENTER_CRITICAL(&s_nonce_mux);
        if (nonce_cache_seen(&s_nonce_cache, announce.nonce)) {
            taskEXIT_CRITICAL(&s_nonce_mux);
            ESP_LOGW(TAG, "ANNOUNCE rejete : nonce deja vu (rejeu, double-check)");
            return;
        }
        nonce_cache_add(&s_nonce_cache, announce.nonce);
        taskEXIT_CRITICAL(&s_nonce_mux);

        evt.type = COMM_EVT_PEER_DISCOVERED;
        memcpy(&evt.data.peer.public_key, &announce.device_key,
               sizeof(public_key_t));
        memcpy(evt.data.peer.mac_addr, src_mac, 6);
        strncpy(evt.data.peer.alias, announce.alias,
                sizeof(evt.data.peer.alias) - 1);

        xQueueSend(config->evt_queue, &evt, 0);
        ESP_LOGI(TAG, "Peer découvert : %s", announce.alias);
        break;
    }

    case COMM_MSG_DISCOVER: {
        /* TODO SECURITE [M6] : Envisager de ne repondre au DISCOVER que pour
         * les peers dans une liste blanche, ou exiger un DISCOVER signe. */

        /*
         * Un autre device cherche des peers. On répond avec un ANNOUNCE
         * signé contenant notre clé publique, un nonce aléatoire et notre alias.
         *
         * [F-EN-009] strnlen au lieu de strlen : own_alias est un
         * char[33] public dans la struct config. Rien ne garantit
         * qu'un appelant n'ait pas rempli les 33 octets sans
         * terminateur null. strlen lirait alors hors-bornes jusqu'a
         * trouver un \0 dans la mémoire adjacente — au mieux on
         * obtient un alias_len absurdement grand qui sature a
         * COMM_MSG_ALIAS_MAX dans pack_announce, au pire on lit
         * de la donnée sensible. strnlen plafonne à sizeof - 1
         * pour rester dans le buffer et garantir la null-termination
         * implicite du résultat (33-1 = 32 = COMM_MSG_ALIAS_MAX).
         */
        size_t  alias_len_full = strnlen(config->own_alias,
                                          sizeof(config->own_alias) - 1);
        uint8_t alias_len = (uint8_t)alias_len_full;

        /* Générer un nonce aléatoire pour l'anti-rejeu */
        uint32_t nonce;
        esp_fill_random(&nonce, sizeof(nonce));

        /* Construire le buffer à signer : [nonce:4][alias_len:1][alias:N] */
        uint8_t sign_buf[4 + 1 + COMM_MSG_ALIAS_MAX];
        size_t sign_len = 4 + 1 + alias_len;
        sign_buf[0] = (uint8_t)(nonce >> 24);
        sign_buf[1] = (uint8_t)(nonce >> 16);
        sign_buf[2] = (uint8_t)(nonce >> 8);
        sign_buf[3] = (uint8_t)(nonce);
        sign_buf[4] = alias_len;
        memcpy(&sign_buf[5], config->own_alias, alias_len);

        /* Signer avec notre clé privée */
        signature_t sig;
        if (crypto_sign(sign_buf, sign_len, config->keypair, &sig) != ESP_OK) {
            ESP_LOGE(TAG, "Erreur signature ANNOUNCE");
            return;
        }

        /* Construire et envoyer le message ANNOUNCE signé */
        uint8_t reply[COMM_MSG_ESPNOW_MAX];
        size_t reply_len;
        if (comm_msg_pack_announce(reply, sizeof(reply),
                                   &config->own_pubkey, &sig, nonce,
                                   config->own_alias, alias_len,
                                   &reply_len) == 0) {
            config->hal->send(src_mac, reply, reply_len, config->hal->ctx);
            ESP_LOGI(TAG, "ANNOUNCE signe envoye en reponse au DISCOVER");
        }
        break;
    }

    case COMM_MSG_TX_LOCKED: {
        /*
         * Un peer nous envoie une transaction verrouillée (paiement).
         * On la transmet au core_task pour validation (signature,
         * solde, etc.). Le core_task décidera d'envoyer un ACK ou non.
         */
        transaction_t tx;
        if (comm_msg_unpack_tx_locked(data, len, &tx) != 0) {
            ESP_LOGW(TAG, "TX_LOCKED malformée");
            return;
        }

        /*
         * [F-EN-004] Anti-rejeu : si la fingerprint 32-bit du tx_id a
         * déjà été vue dans la fenêtre récente, c'est un rejeu —
         * paquet capturé puis ré-émis par un attaquant. Sans cette
         * vérification, le rejeu remplissait silencieusement
         * evt_queue (DoS fonctionnel) et risquait un double-traitement
         * dans core_task. La vérification cryptographique finale reste
         * faite par core_task (validation signature + déduplication
         * par tx_id dans le DAG) ; ce filtre est une défense en
         * profondeur côté couche transport.
         *
         * Pattern double-check identique à ANNOUNCE/ACK : vérif sans
         * lock pour fast-path, puis re-vérif sous lock avant ajout
         * pour fermer la race entre deux receveurs concurrents.
         */
        uint32_t fingerprint = tx_id_fingerprint(&tx.id);

        if (nonce_cache_seen(&s_tx_locked_cache, fingerprint)) {
            ESP_LOGW(TAG, "TX_LOCKED rejeu detecte (fingerprint=0x%08lx)",
                     (unsigned long)fingerprint);
            return;
        }

        taskENTER_CRITICAL(&s_tx_locked_mux);
        if (nonce_cache_seen(&s_tx_locked_cache, fingerprint)) {
            taskEXIT_CRITICAL(&s_tx_locked_mux);
            return;
        }
        nonce_cache_add(&s_tx_locked_cache, fingerprint);
        taskEXIT_CRITICAL(&s_tx_locked_mux);

        evt.type = COMM_EVT_TX_RECEIVED;
        memcpy(&evt.data.tx, &tx, sizeof(transaction_t));

        /* Securite [M7] : conserver l'adresse MAC de l'emetteur dans
         * l'evenement pour que core_task puisse envoyer l'ACK a la
         * bonne adresse sans avoir a la retrouver autrement. */
        memcpy(evt.src_mac, src_mac, 6);
        xQueueSend(config->evt_queue, &evt, 0);
        ESP_LOGI(TAG, "TX LOCKED reçue, montant=%lu",
                 (unsigned long)tx.amount);
        break;
    }

    case COMM_MSG_TX_ACK: {
        /*
         * Le destinataire confirme avoir reçu notre TX verrouillée.
         * On vérifie la signature et le nonce anti-rejeu avant de
         * transmettre au core_task qui confirmera le verrou.
         */
        comm_msg_ack_t ack;
        if (comm_msg_unpack_ack(data, len, &ack) != 0) {
            ESP_LOGW(TAG, "ACK malformé");
            return;
        }

        /* Vérification anti-rejeu sous section critique (même pattern
         * double-check que pour ANNOUNCE, cf. commentaire ci-dessus). */
        taskENTER_CRITICAL(&s_nonce_mux);
        if (nonce_cache_seen(&s_nonce_cache, ack.nonce)) {
            taskEXIT_CRITICAL(&s_nonce_mux);
            ESP_LOGW(TAG, "ACK rejete : nonce deja vu (rejeu)");
            return;
        }
        taskEXIT_CRITICAL(&s_nonce_mux);

        /* Construire le buffer signé : [nonce:4][tx_id:32] */
        uint8_t sign_buf[4 + CRYPTO_HASH_SIZE];
        sign_buf[0] = (uint8_t)(ack.nonce >> 24);
        sign_buf[1] = (uint8_t)(ack.nonce >> 16);
        sign_buf[2] = (uint8_t)(ack.nonce >> 8);
        sign_buf[3] = (uint8_t)(ack.nonce);
        memcpy(&sign_buf[4], ack.tx_id.bytes, CRYPTO_HASH_SIZE);

        /* Vérifier la signature Ed25519 avec la clé publique de l'émetteur
         * (opération coûteuse, effectuée hors section critique) */
        if (crypto_verify(sign_buf, sizeof(sign_buf),
                          &ack.sender_key, &ack.signature) != ESP_OK) {
            ESP_LOGW(TAG, "ACK rejete : signature invalide");
            return;
        }

        /* Double-check sous section critique avant enregistrement du nonce */
        taskENTER_CRITICAL(&s_nonce_mux);
        if (nonce_cache_seen(&s_nonce_cache, ack.nonce)) {
            taskEXIT_CRITICAL(&s_nonce_mux);
            ESP_LOGW(TAG, "ACK rejete : nonce deja vu (rejeu, double-check)");
            return;
        }
        nonce_cache_add(&s_nonce_cache, ack.nonce);
        taskEXIT_CRITICAL(&s_nonce_mux);

        /* [Lot B item 3] Defense en profondeur : verifier que cet ACK
         * correspond a une TX qu'on a effectivement envoyee ET que le
         * signataire de l'ACK est bien la cle "to" qu'on attendait.
         * Cette verification etait deleguee a core_task auparavant — la
         * faire ici en plus reduit la surface d'attaque pour une cle
         * forgee avec signature valide sur un tx_id arbitraire. */
        uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
        if (!pending_tx_consume_and_verify(&ack.tx_id, &ack.sender_key, now_ms)) {
            ESP_LOGW(TAG, "ACK rejete : tx_id inconnu, expire, "
                          "ou signataire != destinataire attendu");
            return;
        }

        evt.type = COMM_EVT_ACK_RECEIVED;
        /* [C4-fix] Propager tx_id ET sender_key. La verification ci-dessus
         * a deja confirme la coherence, mais core_task continue d'effectuer
         * sa propre verification contre la lock_table : c'est volontaire
         * (defense en profondeur). */
        memcpy(&evt.data.ack.tx_id, &ack.tx_id, sizeof(hash_t));
        memcpy(&evt.data.ack.sender_key, &ack.sender_key, sizeof(public_key_t));

        xQueueSend(config->evt_queue, &evt, 0);
        ESP_LOGI(TAG, "ACK reçu pour TX");
        break;
    }

    default:
        /* Messages LoRa reçus sur ESP-NOW : ignorer */
        ESP_LOGD(TAG, "Type de message ignoré sur ESP-NOW : 0x%02x", type);
        break;
    }
}

/* ================================================================
 * Traitement des commandes
 * ================================================================ */

void espnow_handle_cmd(const espnow_config_t *config,
                        const comm_cmd_t *cmd)
{
    if (!config || !cmd) return;

    uint8_t buf[COMM_MSG_ESPNOW_MAX];
    size_t buf_len;

    switch (cmd->type) {

    case COMM_CMD_START_DISCOVER: {
        /*
         * L'UI ou le core_task demande une découverte de peers.
         * On broadcast un message DISCOVER avec notre clé publique.
         * Les réponses ANNOUNCE arriveront via le callback RX.
         */
        if (comm_msg_pack_discover(buf, sizeof(buf),
                                   &config->own_pubkey, &buf_len) == 0) {
            config->hal->broadcast(buf, buf_len, config->hal->ctx);
            ESP_LOGI(TAG, "DISCOVER broadcasté");
        }
        break;
    }

    case COMM_CMD_SEND_TX: {
        /*
         * Le core_task demande d'envoyer une TX verrouillée à un peer.
         * On sérialise la TX et on l'envoie en unicast à la MAC indiquée.
         *
         * [Lot B item 3] Apres l'envoi reseau reussi, on enregistre
         * la TX dans la table des paiements en attente d'ACK. A
         * reception du TX_ACK, on verifiera que le signataire de
         * l'ACK est bien la cle tx.to que nous avons utilisee ici
         * comme destinataire.
         *
         * [F-EN-006] L'enregistrement dans pending_tx_table est fait
         * APRES hal->send pour ne pas occuper l'unique slot 30 s
         * si l'envoi reseau echoue (peer hors portee, erreur
         * esp_now_send). Auparavant, l'ordre inverse bloquait la
         * table pendant 30 s sur un envoi raté, et un retry depuis
         * core_task ecrasait l'entree, faisant perdre l'ACK legitime
         * du premier envoi reussi.
         */
        if (comm_msg_pack_tx_locked(buf, sizeof(buf),
                                    &cmd->data.send_tx.tx,
                                    &buf_len) != 0) {
            ESP_LOGE(TAG, "Échec sérialisation TX LOCKED");
            break;
        }

        hal_err_t send_err = config->hal->send(cmd->data.send_tx.dest_mac,
                                                buf, buf_len,
                                                config->hal->ctx);
        if (send_err != HAL_OK) {
            ESP_LOGW(TAG, "Echec envoi TX LOCKED reseau : %d "
                          "(peer hors portee ?)", (int)send_err);
            /*
             * On NE PAS register dans pending_tx_table : laisser le
             * slot libre pour un retry potentiel. core_task peut
             * detecter le timeout via son propre mecanisme (queue
             * d'ACK qui ne se remplit pas) et decider de retransmettre.
             */
            break;
        }

        uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
        pending_tx_register(&cmd->data.send_tx.tx.id,
                            &cmd->data.send_tx.tx.to,
                            now_ms);
        ESP_LOGI(TAG, "TX LOCKED envoyée, montant=%lu",
                 (unsigned long)cmd->data.send_tx.tx.amount);
        break;
    }

    case COMM_CMD_SEND_ACK: {
        /*
         * Le core_task a validé une TX reçue et demande d'envoyer un ACK
         * signé au peer émetteur pour confirmer le paiement.
         */

        /* Générer un nonce aléatoire pour l'anti-rejeu */
        uint32_t nonce;
        esp_fill_random(&nonce, sizeof(nonce));

        /* Construire le buffer à signer : [nonce:4][tx_id:32] */
        uint8_t sign_buf[4 + CRYPTO_HASH_SIZE];
        sign_buf[0] = (uint8_t)(nonce >> 24);
        sign_buf[1] = (uint8_t)(nonce >> 16);
        sign_buf[2] = (uint8_t)(nonce >> 8);
        sign_buf[3] = (uint8_t)(nonce);
        memcpy(&sign_buf[4], cmd->data.send_ack.tx_id.bytes, CRYPTO_HASH_SIZE);

        /* Signer avec notre clé privée */
        signature_t sig;
        if (crypto_sign(sign_buf, sizeof(sign_buf),
                        config->keypair, &sig) != ESP_OK) {
            ESP_LOGE(TAG, "Erreur signature ACK");
            break;
        }

        /* Construire et envoyer le message ACK signé */
        if (comm_msg_pack_ack(buf, sizeof(buf),
                              &config->own_pubkey, &sig, nonce,
                              &cmd->data.send_ack.tx_id,
                              &buf_len) == 0) {
            config->hal->send(cmd->data.send_ack.dest_mac, buf, buf_len,
                              config->hal->ctx);
            ESP_LOGI(TAG, "ACK signe envoye");
        }
        break;
    }
    }
}

/* ================================================================
 * Tâche FreeRTOS
 * ================================================================ */

void espnow_task(void *param)
{
    espnow_config_t *config = (espnow_config_t *)param;
    if (!config) {
        ESP_LOGE(TAG, "Config NULL, tâche terminée");
        vTaskDelete(NULL);
        return;
    }

    /* Initialiser la HAL ESP-NOW */
    hal_err_t err = config->hal->init(config->hal->ctx);
    if (err != HAL_OK) {
        ESP_LOGE(TAG, "Échec init ESP-NOW HAL : %d", err);
        vTaskDelete(NULL);
        return;
    }

    /* Enregistrer le callback de réception */
    config->hal->set_rx_callback(rx_callback, config, config->hal->ctx);
    ESP_LOGI(TAG, "Tâche ESP-NOW démarrée");

    /*
     * Boucle principale : attendre des commandes avec un court timeout.
     * Le traitement des messages reçus se fait dans le callback RX,
     * qui poste directement les événements dans evt_queue.
     */
    comm_cmd_t cmd;
    for (;;) {
        /* Attendre une commande pendant 100ms max */
        if (xQueueReceive(config->cmd_queue, &cmd, pdMS_TO_TICKS(100)) == pdTRUE) {
            espnow_handle_cmd(config, &cmd);
        }

        /*
         * Ici on pourrait gérer les timeouts de paiement (30s),
         * mais c'est plus propre de le faire dans core_task
         * via lock_table_expire(). La couche comm reste simple.
         */
    }
}
