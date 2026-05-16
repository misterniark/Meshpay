/**
 * @file lora_sync.c
 * @brief Synchronisation LoRa périodique — broadcast et réception.
 *
 * Cycle de sync (~2 minutes, avec jitter) :
 * 0. Au boot, attendre un délai aléatoire dans [0, interval] avant
 *    le 1er cycle (jitter de boot, cf. lora_sync_jitter.h).
 * 1. Acquérir le mutex du DAG
 * 2. Parcourir le DAG pour trouver les TX CONFIRMED récentes
 *    (timestamp > dernier_sync)
 * 3. Relâcher le mutex
 * 4. Sérialiser et envoyer chaque TX via LoRa
 * 5. Retourner en mode réception
 * 6. Dormir un délai aléatoire dans [interval ± 25 %] (jitter par
 *    cycle) avant le prochain.
 *
 * Le double jitter (boot + cycle) décorrèle les émissions de devices
 * co-bootés : sans ça, ils émettent au même tick → collision LoRa →
 * aucun ne reçoit l'autre → DAG ne se propage pas.
 *
 * Réception :
 * - LORA_TX (0x10) : désérialiser → événement LORA_TX_RECEIVED
 * - LORA_FRAG (0x11) : accumuler dans le contexte de réassemblage
 */

#include "comm/lora_sync.h"
#include "comm/comm_msg.h"
#include "comm/lora_frag.h"
#include "comm/lora_tx_packetize.h"
#include "crypto/crypto_sign.h"
#include "transaction/tx_serialize.h"
#include "transaction/tx_validate.h"
#include "lora_sync_jitter.h"  /* helpers PURS de jitter (header privé) */
#include "esp_log.h"
#include "esp_random.h"        /* esp_random() : source d'aléa hardware */
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include <string.h>

/**
 * Pourcentage de variation aléatoire appliqué à chaque cycle de sync :
 * le délai effectif est tiré dans [interval - 25%, interval + 25%].
 * Permet de décorréler les cycles de deux devices bootés simultanément
 * (collision LoRa systématique sinon : tous deux émettent au même tick,
 * aucun ne reçoit l'autre, le DAG ne se propage pas).
 */
#define LORA_SYNC_JITTER_PCT 25U

static const char *TAG = "lora_sync";

/**
 * Contexte de réassemblage statique.
 * Un seul suffit car le LoRa est half-duplex et les séquences
 * de fragmentation sont séquentielles.
 *
 * Protege par un spinlock [H8] car le callback RX peut etre
 * appele depuis le contexte ISR ou une tache differente.
 */
static lora_frag_ctx_t s_frag_ctx;

/*
 * Compteur de séquence pour la fragmentation des TX émises. Incrémenté à
 * chaque TX fragmentée pour que le réassembleur du récepteur distingue
 * deux séquences successives. Le wraparound 255->0 est sans conséquence :
 * deux séquences distinctes ne coexistent jamais assez longtemps pour
 * entrer en collision (timeout de réassemblage : 10 s).
 */
static uint8_t s_lora_tx_seq_id = 0;

/** Spinlock protégeant l'acces concurrent a s_frag_ctx [H8] */
static portMUX_TYPE s_frag_mux = portMUX_INITIALIZER_UNLOCKED;

/*
 * [F-LS-005] Buffer statique de réassemblage final (sortie de
 * lora_frag_get_result). Auparavant alloué sur la pile du callback
 * RX (4 016 octets), il dépassait la pile typique d'une tâche HAL
 * de 4 Ko. En statique, il consomme la RAM une fois pour toutes et
 * laisse la pile libre pour les autres allocations. Accédé uniquement
 * via lora_sync_handle_rx, sous protection du spinlock s_frag_mux
 * tant qu'il est en cours d'écriture par lora_frag_get_result.
 * La copie hors-lock (tx_deserialize, validation Ed25519, xQueueSend)
 * lit ensuite ce buffer en lecture seule jusqu'au prochain fragment.
 */
static uint8_t s_frag_result_buf[LORA_FRAG_MAX_FRAGMENTS * LORA_FRAG_PAYLOAD_MAX];

/**
 * Référence statique vers la config pour le callback RX.
 * Nécessaire car le callback hal_lora ne peut pas porter la config
 * directement (le user_ctx est utilisé pour ça).
 */
static const lora_sync_config_t *s_config_ref = NULL;

/* ================================================================
 * Callback de réception LoRa
 * ================================================================ */

/**
 * Callback appelé par la HAL LoRa quand un paquet est reçu.
 * Transmet au handler pour décodage et traitement.
 */
static void lora_rx_callback(const uint8_t *data, size_t len,
                              int16_t rssi, void *user_ctx)
{
    const lora_sync_config_t *config = (const lora_sync_config_t *)user_ctx;
    if (!config || !data || len == 0) return;

    ESP_LOGD(TAG, "Paquet LoRa reçu (%zu octets, RSSI=%d)", len, rssi);
    lora_sync_handle_rx(config, data, len);
}

/* ================================================================
 * Traitement des paquets reçus
 * ================================================================ */

void lora_sync_handle_rx(const lora_sync_config_t *config,
                          const uint8_t *data, size_t len)
{
    comm_msg_type_t type;
    if (comm_msg_get_type(data, len, &type) != 0) {
        ESP_LOGW(TAG, "Paquet LoRa avec type inconnu");
        return;
    }

    /* Expirer les fragments incomplets [M5] : appeler lora_frag_expire()
     * a chaque reception pour nettoyer les reassemblages abandonnes.
     * Protege par le spinlock [H8]. */
    if (config->get_time) {
        uint64_t current_time = config->get_time();
        taskENTER_CRITICAL(&s_frag_mux);
        lora_frag_expire(&s_frag_ctx, current_time);
        taskEXIT_CRITICAL(&s_frag_mux);
    }

    switch (type) {

    case COMM_MSG_LORA_TX: {
        /*
         * Transaction confirmée unique.
         * Désérialiser, vérifier la signature/hash, puis poster un
         * événement vers core_task.
         */
        transaction_t tx;
        if (comm_msg_unpack_lora_tx(data, len, &tx) != 0) {
            ESP_LOGW(TAG, "LORA_TX malformée");
            return;
        }

        /*
         * [Lot B item 4] Refuser une TX dont la signature Ed25519 ou
         * le hash CBOR ne sont pas valides. Avant ce filtre, n'importe
         * quel emetteur LoRa pouvait injecter une TX forgee dans la queue
         * d'evenements : core_task aurait fini par la rejeter via sa
         * propre validation, mais elle aurait consomme une slot de queue
         * et du temps CPU.
         */
        if (tx_validate_signature(&tx) != ESP_OK) {
            ESP_LOGW(TAG, "LORA_TX rejetee : signature/hash invalide");
            return;
        }

        comm_event_t evt;
        memset(&evt, 0, sizeof(evt));
        evt.type = COMM_EVT_LORA_TX_RECEIVED;
        memcpy(&evt.data.tx, &tx, sizeof(transaction_t));

        xQueueSend(config->evt_queue, &evt, 0);
        ESP_LOGI(TAG, "TX LoRa reçue, montant=%lu",
                 (unsigned long)tx.amount);
        break;
    }

    case COMM_MSG_LORA_FRAG: {
        /*
         * Fragment de rattrapage.
         * Extraire le header et soumettre au réassembleur.
         */
        if (len < LORA_FRAG_HEADER_SIZE) {
            ESP_LOGW(TAG, "Fragment trop court (%zu octets)", len);
            return;
        }

        uint8_t frag_index = data[1];
        uint8_t total      = data[2];
        uint8_t seq_id     = data[3];
        const uint8_t *payload = &data[LORA_FRAG_HEADER_SIZE];
        size_t payload_len = len - LORA_FRAG_HEADER_SIZE;

        /*
         * Utiliser le temps injecté via config pour la gestion du
         * timeout de réassemblage. Si get_time n'est pas configuré,
         * on utilise 0 (pas d'expiration automatique).
         */
        uint64_t frag_time = (config->get_time) ? config->get_time() : 0;

        /*
         * [F-LS-001 + F-LS-002] Section critique réduite au strict
         * minimum : insertion du fragment, extraction du buffer
         * réassemblé si complet, et réinitialisation du contexte.
         * Tout le traitement lourd (tx_deserialize, vérification
         * Ed25519, xQueueSend) est fait HORS section critique.
         *
         * Auparavant, taskENTER_CRITICAL couvrait l'intégralité du
         * traitement : appeler xQueueSend ou exécuter Ed25519 avec
         * les interruptions désactivées est interdit par FreeRTOS
         * et provoque un assert/deadlock dès qu'un fragment complète
         * un réassemblage (cas nominal pour toute TX > 254 octets).
         *
         * La réinitialisation de s_frag_ctx reste DANS la section
         * critique : sans cela, un fragment concurrent avec le même
         * seq_id pourrait écraser le buffer en cours d'extraction
         * (F-LS-002).
         */
        bool complete;
        bool got_result = false;
        /*
         * [F-LS-005] Buffer de réassemblage déplacé en statique
         * (s_frag_result_buf, 4 016 octets) pour libérer la pile du
         * callback RX. La sécurité d'accès repose sur la garantie de
         * séquentialité du callback : la HAL LoRa (driver Core1262 ou
         * Wio-E5) n'invoque lora_rx_callback que depuis sa tâche RX
         * dédiée, sans réentrance. Tant que cette tâche traite un
         * fragment complet (extraction sous lock puis tx_deserialize
         * hors lock), elle ne reçoit pas le suivant — donc pas de
         * risque d'écrasement du buffer pendant le traitement hors
         * section critique.
         *
         * [F-LS-007] result_len est initialisé à 0 pour éliminer le
         * faux positif -Wmaybe-uninitialized qui a conduit à désactiver
         * le warning sur tout le composant (cf. CMakeLists.txt).
         */
        size_t result_len = 0;

        taskENTER_CRITICAL(&s_frag_mux);
        complete = lora_frag_receive(&s_frag_ctx,
                                      frag_index, total, seq_id,
                                      payload, payload_len, frag_time);
        if (complete) {
            if (lora_frag_get_result(&s_frag_ctx, s_frag_result_buf,
                                      sizeof(s_frag_result_buf),
                                      &result_len) == 0) {
                got_result = true;
            }
            /* Réinitialiser le contexte de réassemblage (sous lock). */
            lora_frag_ctx_init(&s_frag_ctx);
        }
        taskEXIT_CRITICAL(&s_frag_mux);

        /*
         * Traitement lourd hors section critique.
         * À ce stade, s_frag_ctx a déjà été réinitialisé et
         * s_frag_result_buf est protégé par la séquentialité du
         * callback RX (cf. commentaire F-LS-005 ci-dessus).
         */
        if (complete) {
            ESP_LOGI(TAG, "Réassemblage complet (%u fragments)", total);
            if (got_result) {
                transaction_t tx;
                if (tx_deserialize(s_frag_result_buf, result_len, &tx) == ESP_OK) {
                    /*
                     * [Lot B item 4] Meme verification de signature qu'en
                     * cas LORA_TX direct, appliquee a la TX reassemblee
                     * a partir des fragments. Sans cela, un attaquant
                     * pouvait fragmenter une TX forgee et la faire entrer
                     * dans la queue d'evenements.
                     */
                    if (tx_validate_signature(&tx) == ESP_OK) {
                        comm_event_t evt;
                        memset(&evt, 0, sizeof(evt));
                        evt.type = COMM_EVT_LORA_TX_RECEIVED;
                        memcpy(&evt.data.tx, &tx, sizeof(transaction_t));
                        xQueueSend(config->evt_queue, &evt, 0);
                    } else {
                        ESP_LOGW(TAG, "LORA_FRAG TX reassemblee rejetee : "
                                      "signature/hash invalide");
                    }
                }
            }
        }
        break;
    }

    case COMM_MSG_LORA_TIME_SYNC: {
        /*
         * Message de synchronisation temporelle d'un maître.
         * Décoder, vérifier la signature Ed25519, puis poster un
         * événement TIME_SYNC_RECEIVED vers core_task.
         */
        comm_msg_time_sync_t sync_msg;
        if (comm_msg_unpack_time_sync(data, len, &sync_msg) != 0) {
            ESP_LOGW(TAG, "LORA_TIME_SYNC malformé");
            return;
        }

        /*
         * Vérifier la signature Ed25519 du maître.
         * La signature couvre [timestamp:8 BE][lamport:8 BE] (16 octets).
         */
        uint8_t sign_buf[16];
        sign_buf[0] = (uint8_t)(sync_msg.master_timestamp >> 56);
        sign_buf[1] = (uint8_t)(sync_msg.master_timestamp >> 48);
        sign_buf[2] = (uint8_t)(sync_msg.master_timestamp >> 40);
        sign_buf[3] = (uint8_t)(sync_msg.master_timestamp >> 32);
        sign_buf[4] = (uint8_t)(sync_msg.master_timestamp >> 24);
        sign_buf[5] = (uint8_t)(sync_msg.master_timestamp >> 16);
        sign_buf[6] = (uint8_t)(sync_msg.master_timestamp >> 8);
        sign_buf[7] = (uint8_t)(sync_msg.master_timestamp);
        sign_buf[8]  = (uint8_t)(sync_msg.master_lamport >> 56);
        sign_buf[9]  = (uint8_t)(sync_msg.master_lamport >> 48);
        sign_buf[10] = (uint8_t)(sync_msg.master_lamport >> 40);
        sign_buf[11] = (uint8_t)(sync_msg.master_lamport >> 32);
        sign_buf[12] = (uint8_t)(sync_msg.master_lamport >> 24);
        sign_buf[13] = (uint8_t)(sync_msg.master_lamport >> 16);
        sign_buf[14] = (uint8_t)(sync_msg.master_lamport >> 8);
        sign_buf[15] = (uint8_t)(sync_msg.master_lamport);

        if (crypto_verify(sign_buf, sizeof(sign_buf),
                          &sync_msg.master_key,
                          &sync_msg.signature) != ESP_OK) {
            ESP_LOGW(TAG, "TIME_SYNC ignore : signature invalide");
            return;
        }

        comm_event_t evt;
        memset(&evt, 0, sizeof(evt));
        evt.type = COMM_EVT_TIME_SYNC_RECEIVED;
        memcpy(&evt.data.time_sync, &sync_msg, sizeof(comm_msg_time_sync_t));

        xQueueSend(config->evt_queue, &evt, 0);
        ESP_LOGI(TAG, "Time sync maître reçu (ts=%llu, lamport=%llu)",
                 (unsigned long long)sync_msg.master_timestamp,
                 (unsigned long long)sync_msg.master_lamport);
        break;
    }

    case COMM_MSG_LORA_BROADCAST: {
        /*
         * Broadcast texte signé d'un maître.
         * Décoder, verifier la signature, puis poster un evenement
         * BROADCAST_RECEIVED vers core_task qui verifiera en plus
         * l'identite "maitre autorise" (mint_authorities).
         */
        comm_msg_broadcast_t bcast_msg;
        if (comm_msg_unpack_broadcast(data, len, &bcast_msg) != 0) {
            ESP_LOGW(TAG, "LORA_BROADCAST malformé");
            return;
        }

        /*
         * [Lot B item 4] Defense en profondeur : valider la signature
         * Ed25519 (msg signe par sender_key) avant d'occuper une slot
         * de queue. La verification d'autorite (cle dans mint_authorities)
         * reste a la charge de core_task : seule la couche applicative
         * connait la liste des maitres autorises.
         */
        if (comm_msg_verify_broadcast(&bcast_msg) != 0) {
            ESP_LOGW(TAG, "LORA_BROADCAST rejete : signature invalide");
            return;
        }

        comm_event_t evt;
        memset(&evt, 0, sizeof(evt));
        evt.type = COMM_EVT_BROADCAST_RECEIVED;
        memcpy(&evt.data.broadcast, &bcast_msg, sizeof(comm_msg_broadcast_t));

        xQueueSend(config->evt_queue, &evt, 0);
        ESP_LOGI(TAG, "Broadcast maître reçu (%u chars)", bcast_msg.text_len);
        break;
    }

    case COMM_MSG_LORA_PING: {
        /*
         * Ping maître signé : découverte des devices à portée LoRa.
         * Décoder, vérifier la signature Ed25519, puis poster un
         * événement PING_RECEIVED vers core_task.
         */
        comm_msg_ping_t ping_msg;
        if (comm_msg_unpack_ping(data, len, &ping_msg) != 0) {
            ESP_LOGW(TAG, "LORA_PING malformé");
            return;
        }

        /*
         * Vérifier la signature Ed25519 du maître.
         * La signature couvre [ping_id:2 BE] (2 octets).
         */
        uint8_t sign_buf[2];
        sign_buf[0] = (uint8_t)(ping_msg.ping_id >> 8);
        sign_buf[1] = (uint8_t)(ping_msg.ping_id);

        if (crypto_verify(sign_buf, sizeof(sign_buf),
                          &ping_msg.master_key,
                          &ping_msg.signature) != ESP_OK) {
            ESP_LOGW(TAG, "PING ignore : signature invalide");
            return;
        }

        comm_event_t evt;
        memset(&evt, 0, sizeof(evt));
        evt.type = COMM_EVT_PING_RECEIVED;
        memcpy(&evt.data.ping, &ping_msg, sizeof(comm_msg_ping_t));

        xQueueSend(config->evt_queue, &evt, 0);
        ESP_LOGD(TAG, "Ping maître reçu (id=%u)", ping_msg.ping_id);
        break;
    }

    case COMM_MSG_LORA_PONG: {
        /*
         * Pong device : réponse à un ping.
         * Décoder et poster vers core_task pour collecte.
         * Les PONGs ne sont PAS relayés.
         *
         * [I4-fix] Vérifier la signature Ed25519 pour empêcher
         * l'usurpation d'identité. Un PONG forgé avec une pubkey
         * tierce produira une signature invalide et sera rejeté.
         */
        comm_msg_pong_t pong_msg;
        if (comm_msg_unpack_pong(data, len, &pong_msg) != 0) {
            ESP_LOGW(TAG, "LORA_PONG malformé");
            return;
        }

        /*
         * Reconstituer le transcript signé : [ping_id:2 BE][alias_len:1][alias:N].
         * Doit correspondre exactement au buffer signé côté émetteur.
         */
        uint8_t sign_buf[2 + 1 + COMM_MSG_ALIAS_MAX];
        size_t  sign_len = 0;
        sign_buf[sign_len++] = (uint8_t)(pong_msg.ping_id >> 8);
        sign_buf[sign_len++] = (uint8_t)(pong_msg.ping_id);
        sign_buf[sign_len++] = pong_msg.alias_len;
        if (pong_msg.alias_len > COMM_MSG_ALIAS_MAX) {
            ESP_LOGW(TAG, "LORA_PONG alias_len hors limites");
            return;
        }
        memcpy(&sign_buf[sign_len], pong_msg.alias, pong_msg.alias_len);
        sign_len += pong_msg.alias_len;

        if (crypto_verify(sign_buf, sign_len,
                          &pong_msg.device_key, &pong_msg.signature) != ESP_OK) {
            ESP_LOGW(TAG, "LORA_PONG rejeté : signature invalide");
            return;
        }

        comm_event_t evt;
        memset(&evt, 0, sizeof(evt));
        evt.type = COMM_EVT_PONG_RECEIVED;
        memcpy(&evt.data.pong, &pong_msg, sizeof(comm_msg_pong_t));

        xQueueSend(config->evt_queue, &evt, 0);
        ESP_LOGD(TAG, "Pong reçu et vérifié (id=%u, alias=%s)",
                 pong_msg.ping_id, pong_msg.alias);
        break;
    }

    case COMM_MSG_LORA_ATTESTATION: {
        /*
         * [I2-fix] Attestation de confirmation signée d'une TX.
         *
         * Décoder, vérifier la signature Ed25519 (le signataire doit
         * pouvoir prouver qu'il a créé cette attestation), puis poster
         * l'événement au core_task qui fera la vérification
         * supplémentaire que l'attester_key est bien tx.to.
         *
         * La signature couvre le tx_id brut (32 octets).
         */
        comm_msg_attestation_t att;
        if (comm_msg_unpack_attestation(data, len, &att) != 0) {
            ESP_LOGW(TAG, "LORA_ATTESTATION malformé");
            return;
        }

        if (crypto_verify(att.tx_id.bytes, CRYPTO_HASH_SIZE,
                          &att.attester_key, &att.signature) != ESP_OK) {
            ESP_LOGW(TAG, "LORA_ATTESTATION rejeté : signature invalide");
            return;
        }

        comm_event_t evt;
        memset(&evt, 0, sizeof(evt));
        evt.type = COMM_EVT_ATTESTATION_RECEIVED;
        memcpy(&evt.data.attestation, &att, sizeof(att));
        xQueueSend(config->evt_queue, &evt, 0);
        ESP_LOGD(TAG, "Attestation reçue et signature OK (tx_id=%02x...)",
                 att.tx_id.bytes[0]);
        break;
    }

    case COMM_MSG_LORA_SET_ALIAS: {
        /*
         * Renommage distant par le maître.
         * Décoder, verifier la signature, puis poster un evenement
         * SET_ALIAS_RECEIVED vers core_task qui verifiera en plus
         * l'identite du maître et que ce device est bien la cible.
         */
        comm_msg_set_alias_t sa_msg;
        if (comm_msg_unpack_set_alias(data, len, &sa_msg) != 0) {
            ESP_LOGW(TAG, "LORA_SET_ALIAS malformé");
            return;
        }

        /*
         * [Lot B item 4] Verifier la signature Ed25519 (msg signe par
         * master_key) avant d'occuper une slot de queue. La verification
         * d'autorite (master_key dans mint_authorities) et de cible
         * (target_key == ma cle) reste a la charge de core_task.
         */
        if (comm_msg_verify_set_alias(&sa_msg) != 0) {
            ESP_LOGW(TAG, "LORA_SET_ALIAS rejete : signature invalide");
            return;
        }

        comm_event_t evt;
        memset(&evt, 0, sizeof(evt));
        evt.type = COMM_EVT_SET_ALIAS_RECEIVED;
        memcpy(&evt.data.set_alias, &sa_msg, sizeof(comm_msg_set_alias_t));

        xQueueSend(config->evt_queue, &evt, 0);
        ESP_LOGD(TAG, "SET_ALIAS reçu (alias=%s, len=%u)",
                 sa_msg.alias, sa_msg.alias_len);
        break;
    }

    case COMM_MSG_LORA_SET_BENEFICIARY: {
        /*
         * Configuration auto-forward bénéficiaire par le maître.
         * Décoder, verifier la signature, puis poster un evenement
         * SET_BENEFICIARY_RECEIVED vers core_task qui verifiera en plus
         * l'identite du maitre et que ce device est la cible.
         */
        comm_msg_set_beneficiary_t sb_msg;
        if (comm_msg_unpack_set_beneficiary(data, len, &sb_msg) != 0) {
            ESP_LOGW(TAG, "LORA_SET_BENEFICIARY malformé");
            return;
        }

        /*
         * [Lot B item 4] Verifier la signature Ed25519 (msg signe par
         * master_key) avant d'occuper une slot de queue. La verification
         * d'autorite et de cible reste a la charge de core_task.
         */
        if (comm_msg_verify_set_beneficiary(&sb_msg) != 0) {
            ESP_LOGW(TAG, "LORA_SET_BENEFICIARY rejete : signature invalide");
            return;
        }

        comm_event_t evt;
        memset(&evt, 0, sizeof(evt));
        evt.type = COMM_EVT_SET_BENEFICIARY_RECEIVED;
        memcpy(&evt.data.set_beneficiary, &sb_msg, sizeof(comm_msg_set_beneficiary_t));

        xQueueSend(config->evt_queue, &evt, 0);
        ESP_LOGD(TAG, "SET_BENEFICIARY reçu (interval=%u min)",
                 sb_msg.forward_interval_min);
        break;
    }

    default:
        ESP_LOGD(TAG, "Type de message ignoré sur LoRa : 0x%02x", type);
        break;
    }
}

/* ================================================================
 * Cycle de synchronisation
 * ================================================================ */

/*
 * Émet une transaction confirmée sur LoRa : un seul paquet LORA_TX si elle
 * tient dans COMM_MSG_LORA_MAX octets, sinon plusieurs fragments LORA_FRAG.
 *
 * Avant ce helper, lora_sync_do_cycle() empaquetait chaque TX dans un
 * buffer de 255 octets et ABANDONNAIT SILENCIEUSEMENT toute TX dont le
 * CBOR dépassait 254 octets — soit toute TX TRANSFER à 2 parents. La
 * fragmentation à l'émission (lora_frag_split) existait mais n'était
 * jamais appelée.
 *
 * LIMITE : le récepteur n'a qu'un seul lora_frag_ctx. Si plusieurs TX
 * fragmentées sont émises dans le même cycle, seule la dernière sera
 * réassemblée ; les précédentes sont abandonnées à l'arrivée du premier
 * fragment de la suivante. Les TX directes (non fragmentées) ne sont pas
 * affectées. Tracé comme dette technique (note Doctech « 07 - Dette
 * technique », entrée « Gossip LoRa fragile ») — à corriger si le volume
 * de TX TRANSFER à 2 parents par cycle devient significatif.
 */
static void lora_sync_send_one_tx(const lora_sync_config_t *config,
                                  const transaction_t *tx,
                                  uint32_t index, uint32_t total)
{
    uint8_t packets[LORA_FRAG_MAX_FRAGMENTS][LORA_FRAG_PACKET_MAX];
    size_t  packet_lens[LORA_FRAG_MAX_FRAGMENTS];
    uint8_t packet_count = 0;

    if (lora_tx_packetize(tx, s_lora_tx_seq_id,
                          packets, packet_lens, &packet_count) != 0) {
        ESP_LOGW(TAG, "TX %lu/%lu non émise : packetize a échoué "
                      "(sérialisation ou > %d fragments)",
                 (unsigned long)(index + 1), (unsigned long)total,
                 LORA_FRAG_MAX_FRAGMENTS);
        return;
    }

    /*
     * Marquer ce seq_id comme utilisé AVANT l'envoi : si l'envoi est
     * partiel, le récepteur a déjà pu voir des fragments portant cet id.
     * Réutiliser le même id dans la TX suivante causerait une collision
     * de réassemblage côté récepteur.
     *
     * [F-LS-004] On incrémente INCONDITIONNELLEMENT, même pour les TX
     * directes (packet_count == 1) qui ne portent pas de seq_id. Cela
     * simplifie le raisonnement (pas de "réutilisation accidentelle"
     * possible) et évite un edge case théorique : avec cycle minimal
     * (90s) et timeout de réassemblage (10s), une suite "frag → direct
     * → frag" pouvait laisser le 2e frag réutiliser le seq_id du 1er
     * dans la fenêtre de timeout. Le wrap-around uint8_t (255 cycles)
     * reste rare en pratique (~6h à 90s/cycle).
     */
    s_lora_tx_seq_id++;

    /*
     * Politique best-effort : on continue d'émettre les fragments suivants
     * même si l'un échoue. Un échec `send()` LoRa est souvent transitoire
     * (collision, CCA) ; abandonner toute la TX sur le premier raté serait
     * pire. Les fragments manquants feront simplement échouer le
     * réassemblage côté récepteur (timeout 10 s), sans collision avec la
     * TX suivante puisque le seq_id a déjà été consommé.
     */
    for (uint8_t p = 0; p < packet_count; p++) {
        hal_err_t err = config->lora->send(packets[p], packet_lens[p],
                                            config->lora->ctx);
        if (err != HAL_OK) {
            ESP_LOGW(TAG, "Échec envoi LoRa TX %lu/%lu (paquet %u/%u)",
                     (unsigned long)(index + 1), (unsigned long)total,
                     (unsigned)(p + 1), (unsigned)packet_count);
        }
    }
}

void lora_sync_do_cycle(const lora_sync_config_t *config,
                         uint64_t *last_sync_ts)
{
    if (!config || !config->collect_confirmed_txs || !last_sync_ts) return;

    /*
     * Si ce device est un maître temporel, broadcaster son temps
     * avant de synchroniser les TX. Le message TIME_SYNC permet aux
     * devices normaux de calibrer leur horloge.
     */
    if (config->is_master && config->get_time &&
        config->own_pubkey && config->own_keypair) {
        uint8_t sync_buf[COMM_MSG_TIME_SYNC_SIZE];
        size_t sync_len;
        uint64_t master_ts = config->get_time();
        uint64_t master_lp = config->get_lamport ? config->get_lamport() : 0;

        /*
         * Signer [timestamp:8 BE][lamport:8 BE] avec la clé privée du maître.
         * Ce format correspond à ce que le récepteur vérifiera.
         */
        uint8_t sign_buf[16];
        sign_buf[0] = (uint8_t)(master_ts >> 56);
        sign_buf[1] = (uint8_t)(master_ts >> 48);
        sign_buf[2] = (uint8_t)(master_ts >> 40);
        sign_buf[3] = (uint8_t)(master_ts >> 32);
        sign_buf[4] = (uint8_t)(master_ts >> 24);
        sign_buf[5] = (uint8_t)(master_ts >> 16);
        sign_buf[6] = (uint8_t)(master_ts >> 8);
        sign_buf[7] = (uint8_t)(master_ts);
        sign_buf[8]  = (uint8_t)(master_lp >> 56);
        sign_buf[9]  = (uint8_t)(master_lp >> 48);
        sign_buf[10] = (uint8_t)(master_lp >> 40);
        sign_buf[11] = (uint8_t)(master_lp >> 32);
        sign_buf[12] = (uint8_t)(master_lp >> 24);
        sign_buf[13] = (uint8_t)(master_lp >> 16);
        sign_buf[14] = (uint8_t)(master_lp >> 8);
        sign_buf[15] = (uint8_t)(master_lp);

        signature_t sig;
        esp_err_t ret = crypto_sign(sign_buf, sizeof(sign_buf),
                                    config->own_keypair, &sig);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Erreur signature TIME_SYNC : %d", ret);
        } else if (comm_msg_pack_time_sync(sync_buf, sizeof(sync_buf),
                                            config->own_pubkey, &sig,
                                            master_ts, master_lp,
                                            &sync_len) == 0) {
            hal_err_t err = config->lora->send(sync_buf, sync_len,
                                                config->lora->ctx);
            if (err != HAL_OK) {
                ESP_LOGW(TAG, "Échec envoi TIME_SYNC");
            } else {
                ESP_LOGI(TAG, "TIME_SYNC signé envoyé (ts=%llu)",
                         (unsigned long long)master_ts);
            }
        }
    }

    /*
     * Buffer temporaire pour stocker les TX à envoyer.
     *
     * [F-LS-006] Réduit de 32 à 8 TX par cycle :
     * - sizeof(transaction_t) ≈ 276 octets (ESP32 alignement typique)
     *   → 8 × 276 = ~2.2 Ko sur la pile, contre 8.8 Ko précédemment
     *   (la tâche LoRa a LORA_TASK_STACK = 24 Ko, mais les buffers de
     *   lora_sync_send_one_tx — packets[16][255] = 4 Ko — s'y ajoutent
     *   sur le même chemin d'appel).
     * - Budget time-on-air EU868 SF9/125kHz : ~330 ms par paquet de
     *   255 octets. 8 TX × 2 fragments × 330 ms ≈ 5.3 s d'émission par
     *   cycle de 120 s — sous le plafond duty cycle 1% (1.2 s/120 s ≈
     *   1%) si les TX restent en un seul paquet et marginal sinon
     *   (à surveiller, cf. Doctech « 07 - Dette technique »).
     *
     * [Lot C item 7] Phase 1 : recuperer les TX CONFIRMED recentes via
     * le callback fourni par main.c. Le callback gere lui-meme le
     * verrouillage applicatif et copie les TX dans tx_to_send. La
     * couche LoRa n'a plus de visibilite sur le DAG ni sur le mutex.
     */
    #define SYNC_MAX_TX_PER_CYCLE 8
    transaction_t tx_to_send[SYNC_MAX_TX_PER_CYCLE];
    uint64_t newest_ts = *last_sync_ts;

    uint32_t tx_count = config->collect_confirmed_txs(*last_sync_ts,
                                                       tx_to_send,
                                                       SYNC_MAX_TX_PER_CYCLE,
                                                       &newest_ts,
                                                       config->collect_ctx);

    /*
     * Phase 2 : envoyer les TX via LoRa (sans mutex).
     *
     * [F-LS-003] On met à jour *last_sync_ts à newest_ts dans TOUS les
     * cas — y compris quand tx_count == 0. Le callback collect_confirmed_txs
     * peut légitimement renseigner newest_ts à une valeur plus récente
     * que since_ts même sans copier de TX (ex. : il a scanné le DAG
     * jusqu'à un timestamp donné mais aucune TX n'était CONFIRMED).
     * Si on retournait sans mise à jour, les TX dont le timestamp est
     * entre l'ancien et le nouveau newest_ts pourraient être perdues
     * dans les cycles suivants (le callback ne les considérera plus
     * comme "nouvelles" puisque since_ts n'a pas avancé, alors qu'il
     * a déjà signalé être passé au-delà).
     *
     * Le contrat de collect_confirmed_txs documenté dans lora_sync.h:53
     * dit "ou since_ts si rien n'a ete copie" — newest_ts est initialisé
     * à *last_sync_ts ligne 650, donc si le callback ne touche pas
     * out_newest_ts, l'assignation finale est un no-op (cohérent).
     */
    if (tx_count == 0) {
        ESP_LOGD(TAG, "Aucune TX à synchroniser");
        *last_sync_ts = newest_ts;
        return;
    }

    ESP_LOGI(TAG, "Sync LoRa : %lu TX à envoyer", (unsigned long)tx_count);

    for (uint32_t i = 0; i < tx_count; i++) {
        lora_sync_send_one_tx(config, &tx_to_send[i], i, tx_count);
    }

    /* Mettre à jour le timestamp de dernière sync */
    *last_sync_ts = newest_ts;
    ESP_LOGI(TAG, "Sync terminée : %lu TX envoyées", (unsigned long)tx_count);
}

/* ================================================================
 * Tâche FreeRTOS
 * ================================================================ */

void lora_sync_task(void *param)
{
    lora_sync_config_t *config = (lora_sync_config_t *)param;
    if (!config) {
        ESP_LOGE(TAG, "Config NULL, tâche terminée");
        vTaskDelete(NULL);
        return;
    }

    /*
     * [F-LS-008] get_time est OBLIGATOIRE : il fournit l'horloge
     * monotonique utilisée par lora_frag_expire/lora_frag_receive
     * pour le timeout de réassemblage des fragments (10 s par défaut).
     * Sans cette fonction, current_time vaut 0 partout, le calcul
     * `current_time - ctx->start_time` reste perpétuellement < TIMEOUT,
     * et un contexte de réassemblage interrompu reste bloqué
     * `active=true` indéfiniment jusqu'à l'arrivée d'un seq_id
     * différent — un seul fragment orphelin paralyse le réassembleur.
     */
    if (!config->get_time) {
        ESP_LOGE(TAG, "Config invalide : get_time obligatoire (F-LS-008)");
        vTaskDelete(NULL);
        return;
    }

    /* Sauvegarder la référence pour le callback RX */
    s_config_ref = config;

    /* Initialiser le contexte de réassemblage (protege par spinlock [H8]) */
    taskENTER_CRITICAL(&s_frag_mux);
    lora_frag_ctx_init(&s_frag_ctx);
    taskEXIT_CRITICAL(&s_frag_mux);

    /* Configurer la HAL LoRa */
    hal_lora_config_t lora_cfg = {
        .frequency_hz     = 868100000,  /* EU868 bande ISM */
        .spreading_factor = 9,          /* SF9 : bon compromis portée/débit */
        .bandwidth        = 0,          /* 125 kHz */
        .coding_rate      = 1,          /* 4/5 */
        .tx_power_dbm     = 14,         /* Max autorisé EU */
    };

    hal_err_t err = config->lora->init(&lora_cfg, config->lora->ctx);
    if (err != HAL_OK) {
        ESP_LOGE(TAG, "Échec init LoRa : %d", err);
        vTaskDelete(NULL);
        return;
    }

    /* Enregistrer le callback RX et activer la réception */
    config->lora->set_rx_callback(lora_rx_callback, (void *)config,
                                   config->lora->ctx);
    config->lora->start_rx(config->lora->ctx);

    /*
     * Jitter de boot : décale le tout premier cycle d'un délai aléatoire
     * dans [0, sync_interval_ms]. Sans ça, deux devices bootés en
     * quasi-simultané (USB hub) entreraient en cycle au même tick →
     * collision LoRa systématique (cf. lora_sync_jitter.h).
     */
    uint32_t boot_delay_ms =
        lora_jitter_initial_ms(config->sync_interval_ms, esp_random());
    ESP_LOGI(TAG,
             "Tâche LoRa sync démarrée (intervalle=%lums, "
             "boot jitter=%lums, jitter cycle=±%u%%)",
             (unsigned long)config->sync_interval_ms,
             (unsigned long)boot_delay_ms,
             (unsigned)LORA_SYNC_JITTER_PCT);

    uint64_t last_sync_ts = 0;

    /* Délai initial AVANT le premier cycle (jitter de boot). */
    vTaskDelay(pdMS_TO_TICKS(boot_delay_ms));

    /* Boucle principale */
    for (;;) {
        /* Effectuer un cycle de synchronisation */
        /*
         * [F-LS-008] Trace l'enchainement do_cycle → start_rx → delay
         * pour diagnostiquer un WDT historique survenant juste apres
         * un cycle LoRa (Interrupt WDT timeout on CPU0 dans
         * espnow_task). Si le firmware crashe entre ces logs, on
         * saura quelle phase a tenu les interrupts trop longtemps.
         */
        ESP_LOGI(TAG, "Cycle: debut do_cycle");
        lora_sync_do_cycle(config, &last_sync_ts);
        ESP_LOGI(TAG, "Cycle: fin do_cycle, appel start_rx...");

        /* Retourner en mode réception */
        hal_err_t rx_err = config->lora->start_rx(config->lora->ctx);
        ESP_LOGI(TAG, "Cycle: start_rx retourne %d, sleep avant prochain cycle", (int)rx_err);

        /*
         * Dormir jusqu'au prochain cycle. Délai aléatoire dans
         * [interval - 25%, interval + 25%] pour maintenir la
         * décorrélation au fil des cycles (deux devices qui auraient
         * fini par re-converger se redécalent).
         */
        uint32_t next_delay_ms =
            lora_jitter_around_ms(config->sync_interval_ms,
                                   LORA_SYNC_JITTER_PCT,
                                   esp_random());
        vTaskDelay(pdMS_TO_TICKS(next_delay_ms));
    }
}
