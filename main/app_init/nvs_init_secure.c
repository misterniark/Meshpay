/**
 * @file app_init/nvs_init_secure.c
 * @brief Init NVS avec chiffrement AES-XTS (compile si CONFIG_NVS_ENCRYPTION=y).
 *
 * Etapes :
 *  1. Trouver la partition `nvs_keys`
 *  2. Obtenir la config de chiffrement via le provider flash-encryption
 *  3. Generer les cles au premier boot, ou les lire ensuite
 *  4. `nvs_flash_secure_init()` (avec effacement si necessaire)
 *  5. Mitigation [C11] : verrouiller le compteur PIN apres effacement
 */

#include "nvs_init.h"

#include "esp_log.h"
#include "esp_partition.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "nvs_sec_provider.h"

static const char *TAG = "nvs_init";

/*
 * Mitigation [C11] : apres effacement NVS, verrouiller le compteur de
 * tentatives PIN a la valeur max. Empeche un attaquant de contourner le
 * brute-force en effacant le NVS (le compteur reviendrait sinon a zero).
 * Necessite un factory reset intentionnel pour deverrouiller.
 */
static void lock_pin_counter_after_erase(void)
{
    nvs_handle_t h;
    if (nvs_open("pin", NVS_READWRITE, &h) == ESP_OK) {
        uint32_t max_fails = 10;
        nvs_set_u32(h, "pin_fails", max_fails);
        nvs_commit(h);
        nvs_close(h);
        ESP_LOGW(TAG, "NVS efface : compteur PIN verrouille (anti brute-force)");
    }
}

esp_err_t nvs_init_storage(bool *out_encrypted)
{
    if (out_encrypted) *out_encrypted = true;
    ESP_LOGI(TAG, "NVS: initialisation avec chiffrement active");

    /* 1. Partition nvs_keys. */
    const esp_partition_t *keys_part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_NVS_KEYS, NULL);
    if (keys_part == NULL) {
        ESP_LOGE(TAG, "Partition nvs_keys introuvable — chiffrement NVS impossible");
        ESP_LOGE(TAG, "Verifiez que partitions.csv contient une partition nvs_keys");
        return ESP_ERR_NOT_FOUND;
    }

    /* 2. Provider flash-encryption. */
    nvs_sec_cfg_t nvs_sec_cfg;
    nvs_sec_scheme_t *sec_scheme = NULL;
    nvs_sec_config_flash_enc_t fe_cfg = NVS_SEC_PROVIDER_CFG_FLASH_ENC_DEFAULT();
    esp_err_t ret = nvs_sec_provider_register_flash_enc(&fe_cfg, &sec_scheme);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Echec enregistrement du scheme NVS sec: 0x%x", ret);
        return ret;
    }

    /* 3. Lire ou generer les cles. */
    ret = nvs_flash_read_security_cfg_v2(sec_scheme, &nvs_sec_cfg);
    if (ret == ESP_ERR_NVS_KEYS_NOT_INITIALIZED) {
        ESP_LOGW(TAG, "NVS: premier boot, generation des cles de chiffrement");
        ret = nvs_flash_generate_keys_v2(sec_scheme, &nvs_sec_cfg);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Echec generation cles NVS: 0x%x", ret);
            return ret;
        }
    } else if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Echec lecture config securite NVS: 0x%x", ret);
        return ret;
    }

    /* 4. Init NVS securisee (avec effacement si necessaire). */
    ret = nvs_flash_secure_init(&nvs_sec_cfg);
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS: effacement et reinitialisation securisee");
        nvs_flash_erase();
        ret = nvs_flash_secure_init(&nvs_sec_cfg);
        if (ret == ESP_OK) {
            lock_pin_counter_after_erase();
        }
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "NVS init echoue: 0x%x", ret);
        return ret;
    }
    return ESP_OK;
}
