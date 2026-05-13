/**
 * @file persistence/nvs_alias.c
 * @brief Implementation alias NVS (voir header).
 */

#include "nvs_alias.h"

#include <stdio.h>

#include "esp_log.h"

#include "app_state.h"
#include "comm/comm_event.h"

static const char *TAG = "nvs_alias";

#define NUM_ADJECTIVES 16
#define NUM_ANIMALS    16

static const char *s_adjectives[NUM_ADJECTIVES] = {
    "Brave", "Vif", "Grand", "Petit", "Doux", "Fort", "Sage", "Fier",
    "Agile", "Calme", "Noble", "Leger", "Rusee", "Clair", "Libre", "Rapide"
};

static const char *s_animals[NUM_ANIMALS] = {
    "Loup", "Renard", "Cerf", "Aigle", "Lynx", "Ours", "Hibou", "Faucon",
    "Herisson", "Loutre", "Belette", "Merle", "Cigale", "Castor", "Lievre", "Cygne"
};

/**
 * Genere un alias deterministe a partir des 2 derniers octets de la pubkey.
 * Reproductible : meme pubkey → meme alias.
 */
static void generate_random_alias(const public_key_t *pubkey,
                                  char *out_buf, uint8_t *out_len)
{
    uint8_t adj_idx = pubkey->bytes[CRYPTO_PUBLIC_KEY_SIZE - 2] % NUM_ADJECTIVES;
    uint8_t ani_idx = pubkey->bytes[CRYPTO_PUBLIC_KEY_SIZE - 1] % NUM_ANIMALS;

    int len = snprintf(out_buf, COMM_MSG_ALIAS_MAX + 1, "%s-%s",
                       s_adjectives[adj_idx], s_animals[ani_idx]);
    if (len < 0) len = 0;
    if (len > COMM_MSG_ALIAS_MAX) len = COMM_MSG_ALIAS_MAX;
    *out_len = (uint8_t)len;
}

esp_err_t nvs_alias_load_or_generate(void)
{
    bool exists = false;
    hal_err_t err = s_storage.exists(NVS_NAMESPACE, NVS_KEY_ALIAS,
                                     &exists, s_storage.ctx);
    if (err != HAL_OK) {
        ESP_LOGE(TAG, "Erreur verification alias NVS: %d", err);
        return ESP_FAIL;
    }

    if (exists) {
        size_t len = COMM_MSG_ALIAS_MAX;
        err = s_storage.blob_read(NVS_NAMESPACE, NVS_KEY_ALIAS,
                                  (uint8_t *)s_device_alias, &len, s_storage.ctx);
        if (err != HAL_OK) {
            ESP_LOGE(TAG, "Erreur lecture alias NVS: %d", err);
            return ESP_FAIL;
        }
        /* [M13] Borner la longueur lue contre une valeur NVS corrompue. */
        if (len > COMM_MSG_ALIAS_MAX) len = COMM_MSG_ALIAS_MAX;
        s_device_alias[len] = '\0';
        s_device_alias_len = (uint8_t)len;
        ESP_LOGI(TAG, "Alias charge depuis NVS: \"%s\"", s_device_alias);
        return ESP_OK;
    }

    /* Premier boot : generer + sauvegarder. */
    generate_random_alias(&s_keypair.public_key, s_device_alias, &s_device_alias_len);
    err = s_storage.blob_write(NVS_NAMESPACE, NVS_KEY_ALIAS,
                               (const uint8_t *)s_device_alias,
                               s_device_alias_len, s_storage.ctx);
    if (err != HAL_OK) {
        ESP_LOGE(TAG, "Erreur ecriture alias NVS: %d", err);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Alias genere et sauvegarde: \"%s\"", s_device_alias);
    return ESP_OK;
}
