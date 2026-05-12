/**
 * @file lora_sync.c
 * @brief Synchronisation LoRa périodique — broadcast et réception.
 *
 * Cycle de sync (toutes les 2 minutes) :
 * 1. Acquérir le mutex du DAG
 * 2. Parcourir le DAG pour trouver les TX CONFIRMED récentes
 *    (timestamp > dernier_sync)
 * 3. Relâcher le mutex
 * 4. Sérialiser et envoyer chaque TX via LoRa
 * 5. Retourner en mode réception
 *
 * Réception :
 * - LORA_TX (0x10) : désérialiser → événement LORA_TX_RECEIVED
 * - LORA_FRAG (0x11) : accumuler dans le contexte de réassemblage
 */

#include "comm/lora_sync.h"
#include "comm/comm_msg.h"
#include "comm/lora_frag.h"
#include "crypto/crypto_sign.h"
#include "transaction/tx_serialize.h"
#include "transaction/tx_validate.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include <string.h>

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

/** Spinlock protégeant l'acces concurrent a s_frag_ctx [H8] */
static portMUX_TYPE s_frag_mux = portMUX_INITIALIZER_UNLOCKED;

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
         *
         * Protection par spinlock [H8] pour tout acces a s_frag_ctx.
         */
        uint64_t frag_time = (config->get_time) ? config->get_time() : 0;

        taskENTER_CRITICAL(&s_frag_mux);
        bool complete = lora_frag_receive(&s_frag_ctx,
                                           frag_index, total, seq_id,
                                           payload, payload_len, frag_time);

        if (complete) {
            ESP_LOGI(TAG, "Réassemblage complet (%u fragments)", total);

            /*
             * Extraire le buffer réassemblé et traiter comme un LORA_TX.
             * Le contenu est une TX sérialisée en CBOR (sans le byte de type).
             */
            uint8_t result_buf[LORA_FRAG_MAX_FRAGMENTS * LORA_FRAG_PAYLOAD_MAX];
            size_t result_len;
            if (lora_frag_get_result(&s_frag_ctx, result_buf,
                                      sizeof(result_buf),
                                      &result_len) == 0) {
                transaction_t tx;
                if (tx_deserialize(result_buf, result_len, &tx) == ESP_OK) {
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

            /* Réinitialiser le contexte de réassemblage */
            lora_frag_ctx_init(&s_frag_ctx);
        }
        taskEXIT_CRITICAL(&s_frag_mux);
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
     * Max 32 TX par cycle pour limiter le temps d'émission radio.
     *
     * [Lot C item 7] Phase 1 : recuperer les TX CONFIRMED recentes via
     * le callback fourni par main.c. Le callback gere lui-meme le
     * verrouillage applicatif et copie les TX dans tx_to_send. La
     * couche LoRa n'a plus de visibilite sur le DAG ni sur le mutex.
     */
    #define SYNC_MAX_TX_PER_CYCLE 32
    transaction_t tx_to_send[SYNC_MAX_TX_PER_CYCLE];
    uint64_t newest_ts = *last_sync_ts;

    uint32_t tx_count = config->collect_confirmed_txs(*last_sync_ts,
                                                       tx_to_send,
                                                       SYNC_MAX_TX_PER_CYCLE,
                                                       &newest_ts,
                                                       config->collect_ctx);

    /* Phase 2 : envoyer les TX via LoRa (sans mutex) */
    if (tx_count == 0) {
        ESP_LOGD(TAG, "Aucune TX à synchroniser");
        return;
    }

    ESP_LOGI(TAG, "Sync LoRa : %lu TX à envoyer", (unsigned long)tx_count);

    uint8_t buf[COMM_MSG_LORA_MAX];
    for (uint32_t i = 0; i < tx_count; i++) {
        size_t buf_len;
        if (comm_msg_pack_lora_tx(buf, sizeof(buf),
                                   &tx_to_send[i], &buf_len) == 0) {
            hal_err_t err = config->lora->send(buf, buf_len,
                                                config->lora->ctx);
            if (err != HAL_OK) {
                ESP_LOGW(TAG, "Échec envoi LoRa TX %lu/%lu",
                         (unsigned long)(i + 1), (unsigned long)tx_count);
            }
        }
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
    ESP_LOGI(TAG, "Tâche LoRa sync démarrée (intervalle=%lums)",
             (unsigned long)config->sync_interval_ms);

    uint64_t last_sync_ts = 0;

    /* Boucle principale */
    for (;;) {
        /* Dormir jusqu'au prochain cycle de sync */
        vTaskDelay(pdMS_TO_TICKS(config->sync_interval_ms));

        /* Effectuer un cycle de synchronisation */
        lora_sync_do_cycle(config, &last_sync_ts);

        /* Retourner en mode réception */
        config->lora->start_rx(config->lora->ctx);
    }
}
