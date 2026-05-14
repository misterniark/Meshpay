/**
 * @file core_task.c
 * @brief Implementation de la boucle d'evenements centrale (voir header).
 *
 * Une seule tache FreeRTOS dans tout le firmware tient le mutex
 * applicatif. Les handlers (handlers) et les ops (ops) sont appeles
 * sous ce mutex. C'est volontairement le SEUL point de synchronisation
 * pour eviter les deadlocks ([C1-fix]).
 */

#include "core_task.h"

#include "esp_log.h"

#include "app_state.h"
#include "balance.h"
#include "handlers/handlers.h"
#include "ops/ops.h"
#include "time_glue.h"
#include "transport/transport_lora.h"
#include "ui_dispatch.h"
#include "wallet/wallet_lock.h"
#include "power_manager.h"

static const char *TAG = "core";

/**
 * @brief Verifie les verrous expires et annule les TX correspondantes.
 *
 * Appelee sous s_state_mutex toutes les LOCK_EXPIRE_INTERVAL_MS.
 */
static void check_lock_expirations(void)
{
    hash_t expired_ids[WALLET_MAX_LOCKS];
    uint32_t expired_count = 0;
    lock_table_expire(&s_lock_table, expired_ids, WALLET_MAX_LOCKS, &expired_count);
    for (uint32_t i = 0; i < expired_count; i++) {
        dag_set_status(&s_dag, &expired_ids[i], TX_STATUS_CANCELLED);
        ESP_LOGW(TAG, "Verrou expire, TX annulee");
    }
}

void core_task(void *param)
{
    (void)param;
    ESP_LOGI(TAG, "core_task demarre");

    comm_event_t evt;

    for (;;) {
        /* Attente d'un evenement (timeout 1 s). */
        BaseType_t got = xQueueReceive(s_evt_queue, &evt, pdMS_TO_TICKS(1000));

        /*
         * Mutex applicatif. Timeout 5 s : si on ne peut pas le prendre,
         * c'est qu'une autre tache le tient anormalement longtemps. Log
         * et retry au prochain cycle.
         */
        if (xSemaphoreTake(s_state_mutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
            ESP_LOGE(TAG, "core_task: impossible de prendre le mutex");
            continue;
        }

        if (got == pdTRUE) {
            switch (evt.type) {
                case COMM_EVT_PEER_DISCOVERED:
                    handle_peer_discovered(&evt); break;
                case COMM_EVT_TX_RECEIVED:
                case COMM_EVT_LORA_TX_RECEIVED:
                    handle_tx_received(&evt); break;
                case COMM_EVT_ACK_RECEIVED:
                    handle_ack_received(&evt); break;
                case COMM_EVT_TX_TIMEOUT:
                    handle_tx_timeout(&evt); break;
                case COMM_EVT_TIME_SYNC_RECEIVED:
                    handle_time_sync(&evt); break;
                case COMM_EVT_BROADCAST_RECEIVED:
                    handle_broadcast_received(&evt); break;
                case COMM_EVT_PING_RECEIVED:
                    handle_ping_received(&evt); break;
                case COMM_EVT_PONG_RECEIVED:
                    handle_pong_received(&evt); break;
                case COMM_EVT_SET_ALIAS_RECEIVED:
                    handle_set_alias_received(&evt); break;
                case COMM_EVT_SET_BENEFICIARY_RECEIVED:
                    handle_set_beneficiary_received(&evt); break;
                case COMM_EVT_ATTESTATION_RECEIVED:
                    handle_attestation_received(&evt); break;
                default:
                    ESP_LOGW(TAG, "Evenement inconnu: %d", evt.type);
                    break;
            }
        }

        /* Drainer les commandes UI (non-bloquant). */
        {
            ui_cmd_t ui_cmd;
            while (xQueueReceive(s_ui_cmd_queue, &ui_cmd, 0) == pdTRUE) {
                handle_ui_command(&ui_cmd);
            }
        }

        /* Verification periodique des expirations de verrous. */
        uint64_t now = get_time_ms_wrapper();
        if (now - s_last_expire_check >= LOCK_EXPIRE_INTERVAL_MS) {
            check_lock_expirations();
            s_last_expire_check = now;
        }

        /* Forward periodique vers le beneficiaire si actif. */
        if (s_forward_interval_min > 0) {
            uint64_t interval_ms = (uint64_t)s_forward_interval_min * 60000;
            if (now - s_last_forward_ms >= interval_ms) {
                attempt_beneficiary_forward();
                s_last_forward_ms = now;
            }
        }

        /*
         * Application periodique de la fonte si activee. Met a jour
         * last_melt_timestamp meme sans transaction, evitant un rattrapage
         * massif de ticks apres une longue inactivite.
         */
        if (s_currency.melt_enabled) {
            uint32_t melt_balance = 0;
            compute_owner_balance(&melt_balance);
            apply_pending_melt(&melt_balance);
        }

        xSemaphoreGive(s_state_mutex);

        /* Gestion de l'energie (feature 13) — hors mutex applicatif :
         * power_manager a son propre verrou. */
        if (got == pdTRUE) {
            /* Un evenement reseau est une interaction reseau. */
            power_manager_notify_activity();
        }
        power_manager_tick();

        /*
         * Hors mutex : pomper les envois LoRa differes (relay
         * broadcast/ping + PONG signe). No-op sur cibles sans LoRa.
         */
        transport_lora_pump();
    }
}
