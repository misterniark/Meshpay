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
#include "crypto/crypto_sign.h"

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

    /*
     * [F-MN-012] Vérification de la signature Ed25519 du message.
     *
     * Sans cette vérification, n'importe quel attaquant pouvant émettre
     * sur le canal LoRa pouvait forger un TIME_SYNC avec une clé
     * publique d'autorité connue (information publique hardcodée dans
     * la config) et injecter un timestamp Lamport arbitraire —
     * désynchronisation totale du ledger.
     *
     * Le payload signé est `[timestamp:8 BE][lamport:8 BE]`, identique
     * à ce que pack le maître émetteur (cf. `comm_msg_pack_time_sync`).
     */
    uint8_t signed_payload[16];
    uint64_t ts = sync->master_timestamp;
    uint64_t lp = sync->master_lamport;
    for (int i = 0; i < 8; i++) {
        signed_payload[i]     = (uint8_t)(ts >> (56 - i * 8));
        signed_payload[8 + i] = (uint8_t)(lp >> (56 - i * 8));
    }

    esp_err_t verr = crypto_verify(signed_payload, sizeof(signed_payload),
                                   &sync->master_key, &sync->signature);
    if (verr != ESP_OK) {
        ESP_LOGW(TAG, "TIME_SYNC rejete : signature invalide");
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
