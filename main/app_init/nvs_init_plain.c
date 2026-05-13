/**
 * @file app_init/nvs_init_plain.c
 * @brief Init NVS sans chiffrement (compile si CONFIG_NVS_ENCRYPTION=n).
 *
 * Mode dev/proto uniquement. En production, activer Flash Encryption +
 * CONFIG_NVS_ENCRYPTION pour passer sur nvs_init_secure.c.
 */

#include "nvs_init.h"

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "nvs_init";

/* Voir nvs_init_secure.c pour le rationnel [C11]. */
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
    if (out_encrypted) *out_encrypted = false;
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS: effacement et reinitialisation");
        nvs_flash_erase();
        ret = nvs_flash_init();
        if (ret == ESP_OK) {
            lock_pin_counter_after_erase();
        }
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "NVS init echoue: 0x%x", ret);
    }
    return ret;
}
