/**
 * @file handlers/handler_admin.c
 * @brief Reception des commandes admin du maitre : SET_ALIAS, SET_BENEFICIARY.
 *
 * Verifications communes :
 *  1. Emetteur dans mint_authorities
 *  2. Signature Ed25519 valide
 *  3. target_key == notre propre pubkey
 *
 * Effet de bord : maj de l'etat local + NVS.
 */

#include "handlers.h"

#include <string.h>

#include "esp_log.h"

#include "app_state.h"
#include "comm/comm_msg.h"
#include "crypto/crypto_sign.h"
#include "time_glue.h"

static const char *TAG = "h_admin";

void handle_set_alias_received(const comm_event_t *evt)
{
    const comm_msg_set_alias_t *sa = &evt->data.set_alias;

    /* [M13] Borne alias_len contre un debordement de buffer. */
    if (sa->alias_len > COMM_MSG_ALIAS_MAX) {
        ESP_LOGW(TAG, "SET_ALIAS ignore : alias_len=%u depasse le max %u",
                 sa->alias_len, COMM_MSG_ALIAS_MAX);
        return;
    }

    bool is_master = false;
    for (uint32_t i = 0; i < s_currency.mint_authority_count; i++) {
        if (public_key_equal(&sa->master_key,
                             &s_currency.mint_authorities[i])) {
            is_master = true;
            break;
        }
    }
    if (!is_master) {
        ESP_LOGW(TAG, "SET_ALIAS ignore : emetteur non autorise");
        return;
    }

    /* Signature couvre [target_key:32][alias_len:1][alias:N]. */
    uint8_t signed_data[CRYPTO_PUBLIC_KEY_SIZE + 1 + COMM_MSG_ALIAS_MAX];
    memcpy(signed_data, sa->target_key.bytes, CRYPTO_PUBLIC_KEY_SIZE);
    signed_data[CRYPTO_PUBLIC_KEY_SIZE] = sa->alias_len;
    memcpy(&signed_data[CRYPTO_PUBLIC_KEY_SIZE + 1], sa->alias, sa->alias_len);
    size_t signed_len = CRYPTO_PUBLIC_KEY_SIZE + 1 + sa->alias_len;

    esp_err_t ret = crypto_verify(signed_data, signed_len,
                                  &sa->master_key, &sa->signature);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "SET_ALIAS ignore : signature invalide");
        return;
    }
    if (!public_key_equal(&sa->target_key, &s_keypair.public_key)) {
        ESP_LOGD(TAG, "SET_ALIAS ignore : cible differente");
        return;
    }

    memcpy(s_device_alias, sa->alias, sa->alias_len);
    s_device_alias[sa->alias_len] = '\0';
    s_device_alias_len = sa->alias_len;

    hal_err_t herr = s_storage.blob_write(NVS_NAMESPACE, NVS_KEY_ALIAS,
                                          (const uint8_t *)s_device_alias,
                                          s_device_alias_len, s_storage.ctx);
    if (herr != HAL_OK) {
        ESP_LOGW(TAG, "Echec sauvegarde alias NVS (err=%d)", herr);
    }
    ESP_LOGI(TAG, "Alias mis a jour par le maitre : \"%s\"", s_device_alias);
}

void handle_set_beneficiary_received(const comm_event_t *evt)
{
    const comm_msg_set_beneficiary_t *sb = &evt->data.set_beneficiary;

    bool is_master = false;
    for (uint32_t i = 0; i < s_currency.mint_authority_count; i++) {
        if (public_key_equal(&sb->master_key,
                             &s_currency.mint_authorities[i])) {
            is_master = true;
            break;
        }
    }
    if (!is_master) {
        ESP_LOGW(TAG, "SET_BENEFICIARY ignore : emetteur non autorise");
        return;
    }

    /* Signature couvre [target_key:32][beneficiary_key:32][interval:2 BE]. */
    uint8_t signed_data[CRYPTO_PUBLIC_KEY_SIZE + CRYPTO_PUBLIC_KEY_SIZE + 2];
    size_t offset = 0;
    memcpy(&signed_data[offset], sb->target_key.bytes, CRYPTO_PUBLIC_KEY_SIZE);
    offset += CRYPTO_PUBLIC_KEY_SIZE;
    memcpy(&signed_data[offset], sb->beneficiary_key.bytes, CRYPTO_PUBLIC_KEY_SIZE);
    offset += CRYPTO_PUBLIC_KEY_SIZE;
    signed_data[offset]     = (uint8_t)(sb->forward_interval_min >> 8);
    signed_data[offset + 1] = (uint8_t)(sb->forward_interval_min);

    esp_err_t ret = crypto_verify(signed_data, sizeof(signed_data),
                                  &sb->master_key, &sb->signature);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "SET_BENEFICIARY ignore : signature invalide");
        return;
    }
    if (!public_key_equal(&sb->target_key, &s_keypair.public_key)) {
        ESP_LOGD(TAG, "SET_BENEFICIARY ignore : cible differente");
        return;
    }

    if (public_key_is_zero(&sb->beneficiary_key)) {
        memset(&s_beneficiary_key, 0, sizeof(public_key_t));
        s_forward_interval_min = 0;
        s_storage.blob_write(NVS_NAMESPACE, NVS_KEY_BENEFICIARY,
                             s_beneficiary_key.bytes, CRYPTO_PUBLIC_KEY_SIZE,
                             s_storage.ctx);
        s_storage.u32_write(NVS_NAMESPACE, NVS_KEY_FWD_INTERVAL,
                            0, s_storage.ctx);
        ESP_LOGI(TAG, "Auto-forward desactive par le maitre");
        return;
    }

    memcpy(&s_beneficiary_key, &sb->beneficiary_key, sizeof(public_key_t));
    s_forward_interval_min = sb->forward_interval_min;
    /* Plancher a 1 minute pour eviter le spam de TX. */
    if (s_forward_interval_min < 1) s_forward_interval_min = 1;

    s_storage.blob_write(NVS_NAMESPACE, NVS_KEY_BENEFICIARY,
                         s_beneficiary_key.bytes, CRYPTO_PUBLIC_KEY_SIZE,
                         s_storage.ctx);
    s_storage.u32_write(NVS_NAMESPACE, NVS_KEY_FWD_INTERVAL,
                        (uint32_t)s_forward_interval_min, s_storage.ctx);

    /* Reset timer pour eviter un forward immediat. */
    s_last_forward_ms = get_time_ms_wrapper();

    ESP_LOGI(TAG, "Auto-forward active : interval=%u min",
             s_forward_interval_min);
}
