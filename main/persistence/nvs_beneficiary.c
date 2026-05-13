/**
 * @file persistence/nvs_beneficiary.c
 * @brief Implementation chargement beneficiaire (voir header).
 */

#include "nvs_beneficiary.h"

#include "esp_log.h"

#include "app_state.h"

static const char *TAG = "nvs_ben";

esp_err_t nvs_beneficiary_load(void)
{
    bool exists = false;
    hal_err_t herr = s_storage.exists(NVS_NAMESPACE, NVS_KEY_BENEFICIARY,
                                      &exists, s_storage.ctx);
    if (herr != HAL_OK || !exists) {
        /* Pas de beneficiaire configure : valeur par defaut (inactif). */
        return ESP_OK;
    }

    size_t key_len = sizeof(public_key_t);
    herr = s_storage.blob_read(NVS_NAMESPACE, NVS_KEY_BENEFICIARY,
                               s_beneficiary_key.bytes, &key_len,
                               s_storage.ctx);
    if (herr != HAL_OK || key_len != CRYPTO_PUBLIC_KEY_SIZE) {
        ESP_LOGW(TAG, "Cle beneficiaire NVS invalide (len=%zu), ignoree", key_len);
        return ESP_OK;
    }

    uint32_t interval = 0;
    herr = s_storage.u32_read(NVS_NAMESPACE, NVS_KEY_FWD_INTERVAL,
                              &interval, s_storage.ctx);
    if (herr == HAL_OK && interval > 0) {
        s_forward_interval_min = (uint16_t)interval;
        ESP_LOGI(TAG, "Auto-forward charge: interval=%u min",
                 s_forward_interval_min);
    }
    return ESP_OK;
}
