/**
 * @file persistence/nvs_checkpoint.c
 * @brief Implementation persistance checkpoint (voir header).
 */

#include "nvs_checkpoint.h"

#include <inttypes.h>

#include "esp_log.h"

#include "app_state.h"

static const char *TAG = "nvs_chk";

esp_err_t nvs_checkpoint_load(checkpoint_t *checkpoint, void *ctx)
{
    (void)ctx;
    size_t len = sizeof(checkpoint_t);
    bool exists = false;

    hal_err_t err = s_storage.exists(NVS_NAMESPACE, NVS_KEY_CHECKPOINT,
                                     &exists, s_storage.ctx);
    if (err != HAL_OK || !exists) {
        return ESP_ERR_NOT_FOUND;
    }

    size_t stored_len = len;
    err = s_storage.blob_read(NVS_NAMESPACE, NVS_KEY_CHECKPOINT,
                              (uint8_t *)checkpoint, &stored_len, s_storage.ctx);
    if (err != HAL_OK) {
        ESP_LOGE(TAG, "Erreur lecture checkpoint NVS: %d", err);
        return ESP_FAIL;
    }

    /*
     * Migration : si le checkpoint NVS est d'un ancien format (avant
     * ajout de last_melt_timestamp), ce champ contient des donnees
     * aleatoires. Reinitialiser a 0 pour que la fonte demarre proprement.
     */
    if (stored_len < sizeof(checkpoint_t)) {
        ESP_LOGW(TAG, "Checkpoint ancien format (%zu < %zu), "
                 "last_melt_timestamp remis a 0",
                 stored_len, sizeof(checkpoint_t));
        checkpoint->last_melt_timestamp = 0;
    }

    ESP_LOGI(TAG, "Checkpoint charge (%"PRIu32" comptes)",
             checkpoint->account_count);
    return ESP_OK;
}

esp_err_t nvs_checkpoint_save(const checkpoint_t *checkpoint, void *ctx)
{
    (void)ctx;
    hal_err_t err = s_storage.blob_write(NVS_NAMESPACE, NVS_KEY_CHECKPOINT,
                                         (const uint8_t *)checkpoint,
                                         sizeof(checkpoint_t), s_storage.ctx);
    if (err != HAL_OK) {
        ESP_LOGE(TAG, "Erreur ecriture checkpoint NVS: %d", err);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Checkpoint sauvegarde (%"PRIu32" comptes)",
             checkpoint->account_count);
    return ESP_OK;
}
