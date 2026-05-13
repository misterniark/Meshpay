/**
 * @file persistence/nvs_keypair.c
 * @brief Implementation du load/generate keypair (voir header).
 */

#include "nvs_keypair.h"

#include "esp_log.h"

#include "app_state.h"
#include "crypto/crypto_keys.h"

static const char *TAG = "nvs_kp";

esp_err_t nvs_keypair_load_or_generate(void)
{
    size_t len = sizeof(s_keypair.private_key);
    bool key_exists = false;

    hal_err_t err = s_storage.exists(NVS_NAMESPACE, NVS_KEY_PRIVKEY,
                                     &key_exists, s_storage.ctx);
    if (err != HAL_OK) {
        ESP_LOGE(TAG, "Erreur verification cle NVS: %d", err);
        return ESP_FAIL;
    }

    if (key_exists) {
        err = s_storage.blob_read(NVS_NAMESPACE, NVS_KEY_PRIVKEY,
                                  s_keypair.private_key, &len, s_storage.ctx);
        if (err != HAL_OK) {
            ESP_LOGE(TAG, "Erreur lecture cle privee NVS: %d", err);
            return ESP_FAIL;
        }
        len = sizeof(s_keypair.public_key);
        err = s_storage.blob_read(NVS_NAMESPACE, NVS_KEY_PUBKEY,
                                  s_keypair.public_key.bytes, &len, s_storage.ctx);
        if (err != HAL_OK) {
            ESP_LOGE(TAG, "Erreur lecture cle publique NVS: %d", err);
            return ESP_FAIL;
        }
        ESP_LOGI(TAG, "Keypair charge depuis NVS");
        return ESP_OK;
    }

    /* Premier boot : generer + sauvegarder. */
    esp_err_t ret = crypto_generate_keypair(&s_keypair);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Erreur generation keypair: %d", ret);
        return ret;
    }

    err = s_storage.blob_write(NVS_NAMESPACE, NVS_KEY_PRIVKEY,
                               s_keypair.private_key,
                               sizeof(s_keypair.private_key), s_storage.ctx);
    if (err != HAL_OK) {
        ESP_LOGE(TAG, "Erreur ecriture cle privee NVS: %d", err);
        return ESP_FAIL;
    }
    err = s_storage.blob_write(NVS_NAMESPACE, NVS_KEY_PUBKEY,
                               s_keypair.public_key.bytes,
                               sizeof(s_keypair.public_key), s_storage.ctx);
    if (err != HAL_OK) {
        ESP_LOGE(TAG, "Erreur ecriture cle publique NVS: %d", err);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Nouveau keypair genere et sauvegarde");
    return ESP_OK;
}
