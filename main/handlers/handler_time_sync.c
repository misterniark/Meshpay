/**
 * @file handlers/handler_time_sync.c
 * @brief Reception d'un TIME_SYNC du maitre via LoRa.
 *
 * Verifie que l'emetteur est une mint_authority avant d'accepter
 * l'horodatage. Sans cela un attaquant pourrait desynchroniser les
 * horloges Lamport de tout le reseau.
 */

#include "handlers.h"

#include "esp_log.h"

#include "app_state.h"

static const char *TAG = "h_time";

void handle_time_sync(const comm_event_t *evt)
{
    const comm_msg_time_sync_t *sync = &evt->data.time_sync;

    bool is_authorized = false;
    for (uint32_t i = 0; i < s_currency.mint_authority_count; i++) {
        if (public_key_equal(&sync->master_key,
                             &s_currency.mint_authorities[i])) {
            is_authorized = true;
            break;
        }
    }
    if (!is_authorized) {
        ESP_LOGW(TAG, "TIME_SYNC rejete : emetteur non autorise");
        return;
    }

    int ret = time_manager_on_master_sync(&s_time_manager,
                                          &sync->master_key,
                                          sync->master_timestamp,
                                          sync->master_lamport);
    if (ret == 0) {
        ESP_LOGI(TAG, "Time sync accepte");
    } else {
        ESP_LOGD(TAG, "Time sync rejete (%d)", ret);
    }
}
