/**
 * @file ops/op_master.c
 * @brief Operations reservees au maitre (mint_authority) : broadcast,
 *        ping, set_alias, set_beneficiary.
 *
 * Chaque op verifie `is_master` (device dans `s_currency.mint_authorities`)
 * et retourne `ESP_ERR_NOT_ALLOWED` sur non-maitre.
 *
 * Sur d'eventuelles cibles sans Wio-E5, `transport_lora_send` est no-op
 * (stub) : le maitre ne peut pas emettre en LoRa, ce qui est le
 * comportement attendu. CYD et S3 Waveshare ont tous deux un Wio-E5.
 *
 * Compilent partout — pas de garde `#if CONFIG_IDF_TARGET_ESP32`.
 */

#include "ops.h"

#include <string.h>

#include "esp_log.h"

#include "app_state.h"
#include "comm/comm_msg.h"
#include "crypto/crypto_sign.h"
#include "handlers/handlers.h"  /* pour ping_mark_seen_public */
#include "transport/transport_lora.h"

static const char *TAG = "op_master";

/* ----------------------------------------------------------------
 * Helper interne : ce device est-il maitre ?
 * ---------------------------------------------------------------- */
static bool is_self_master(void)
{
    for (uint32_t i = 0; i < s_currency.mint_authority_count; i++) {
        if (public_key_equal(&s_keypair.public_key,
                             &s_currency.mint_authorities[i])) {
            return true;
        }
    }
    return false;
}

/* ----------------------------------------------------------------
 * broadcast_text_send : envoi d'un message texte signe a tous les
 *                       devices LoRa.
 * ---------------------------------------------------------------- */
esp_err_t broadcast_text_send(const char *text, uint8_t text_len)
{
    if (!text || text_len == 0 || text_len > COMM_MSG_BROADCAST_TEXT_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!is_self_master()) {
        ESP_LOGW(TAG, "broadcast_text_send: device non maitre");
        return ESP_ERR_NOT_ALLOWED;
    }

    /* Signature couvre [text_len:1][text:N]. */
    uint8_t signed_data[1 + COMM_MSG_BROADCAST_TEXT_MAX];
    signed_data[0] = text_len;
    memcpy(&signed_data[1], text, text_len);
    size_t signed_len = 1 + text_len;

    signature_t sig;
    esp_err_t ret = crypto_sign(signed_data, signed_len, &s_keypair, &sig);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Erreur signature broadcast: %d", ret);
        return ret;
    }

    uint8_t buf[COMM_MSG_LORA_MAX];
    size_t out_len;
    if (comm_msg_pack_broadcast(buf, sizeof(buf),
                                &s_keypair.public_key, &sig,
                                text, text_len, &out_len) != 0) {
        ESP_LOGE(TAG, "Erreur pack broadcast");
        return ESP_FAIL;
    }
    if (!transport_lora_send(buf, out_len, "broadcast")) {
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Broadcast envoye (%u chars): \"%.*s\"",
             text_len, text_len, text);
    return ESP_OK;
}

/* ----------------------------------------------------------------
 * ping_send : reset les resultats et envoie un PING signe via LoRa.
 *             L'UI consultera s_ping_results[] apres quelques secondes.
 * ---------------------------------------------------------------- */
esp_err_t ping_send(void)
{
    s_current_ping_id++;
    s_ping_result_count = 0;
    memset(s_ping_results, 0, sizeof(s_ping_results));

    /* Marquer notre propre PING comme vu (anti-boucle si on recoit notre relay). */
    ping_mark_seen_public(&s_keypair.public_key, s_current_ping_id);

    uint8_t sign_buf[2];
    sign_buf[0] = (uint8_t)(s_current_ping_id >> 8);
    sign_buf[1] = (uint8_t)(s_current_ping_id);

    signature_t sig;
    esp_err_t ret = crypto_sign(sign_buf, sizeof(sign_buf), &s_keypair, &sig);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Erreur signature PING: %d", ret);
        return ret;
    }

    uint8_t buf[COMM_MSG_PING_SIZE];
    size_t out_len;
    if (comm_msg_pack_ping(buf, sizeof(buf),
                           &s_keypair.public_key, &sig,
                           s_current_ping_id, &out_len) != 0) {
        ESP_LOGE(TAG, "Erreur pack PING");
        return ESP_FAIL;
    }
    if (!transport_lora_send(buf, out_len, "PING")) {
        return ESP_FAIL;
    }

    s_ping_active = true;
    ESP_LOGI(TAG, "PING signe envoye (id=%u), collecte PONGs...",
             s_current_ping_id);
    return ESP_OK;
}

/* ----------------------------------------------------------------
 * set_alias_send : renomme un device distant via LoRa.
 * ---------------------------------------------------------------- */
esp_err_t set_alias_send(const public_key_t *target_key,
                         const char *alias, uint8_t alias_len)
{
    if (!target_key || !alias || alias_len == 0 || alias_len > COMM_MSG_ALIAS_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!is_self_master()) {
        ESP_LOGW(TAG, "set_alias_send: device non maitre");
        return ESP_ERR_NOT_ALLOWED;
    }

    /* Signature couvre [target_key:32][alias_len:1][alias:N]. */
    uint8_t signed_data[CRYPTO_PUBLIC_KEY_SIZE + 1 + COMM_MSG_ALIAS_MAX];
    memcpy(signed_data, target_key->bytes, CRYPTO_PUBLIC_KEY_SIZE);
    signed_data[CRYPTO_PUBLIC_KEY_SIZE] = alias_len;
    memcpy(&signed_data[CRYPTO_PUBLIC_KEY_SIZE + 1], alias, alias_len);
    size_t signed_len = CRYPTO_PUBLIC_KEY_SIZE + 1 + alias_len;

    signature_t sig;
    esp_err_t ret = crypto_sign(signed_data, signed_len, &s_keypair, &sig);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Erreur signature SET_ALIAS: %d", ret);
        return ret;
    }

    uint8_t buf[COMM_MSG_LORA_MAX];
    size_t out_len;
    if (comm_msg_pack_set_alias(buf, sizeof(buf),
                                &s_keypair.public_key, &sig,
                                target_key, alias, alias_len,
                                &out_len) != 0) {
        ESP_LOGE(TAG, "Erreur pack SET_ALIAS");
        return ESP_FAIL;
    }
    if (!transport_lora_send(buf, out_len, "SET_ALIAS")) {
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "SET_ALIAS envoye : \"%.*s\"", alias_len, alias);
    return ESP_OK;
}

/* ----------------------------------------------------------------
 * set_beneficiary_send : configure l'auto-forward d'un device distant.
 *                        Si beneficiary_key all-zeros, desactive le mode.
 * ---------------------------------------------------------------- */
esp_err_t set_beneficiary_send(const public_key_t *target_key,
                               const public_key_t *beneficiary_key,
                               uint16_t forward_interval_min)
{
    if (!target_key || !beneficiary_key) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!is_self_master()) {
        ESP_LOGW(TAG, "set_beneficiary_send: device non maitre");
        return ESP_ERR_NOT_ALLOWED;
    }

    /* Signature couvre [target_key:32][beneficiary_key:32][interval:2 BE]. */
    uint8_t signed_data[CRYPTO_PUBLIC_KEY_SIZE + CRYPTO_PUBLIC_KEY_SIZE + 2];
    size_t offset = 0;
    memcpy(&signed_data[offset], target_key->bytes, CRYPTO_PUBLIC_KEY_SIZE);
    offset += CRYPTO_PUBLIC_KEY_SIZE;
    memcpy(&signed_data[offset], beneficiary_key->bytes, CRYPTO_PUBLIC_KEY_SIZE);
    offset += CRYPTO_PUBLIC_KEY_SIZE;
    signed_data[offset]     = (uint8_t)(forward_interval_min >> 8);
    signed_data[offset + 1] = (uint8_t)(forward_interval_min);

    signature_t sig;
    esp_err_t ret = crypto_sign(signed_data, sizeof(signed_data), &s_keypair, &sig);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Erreur signature SET_BENEFICIARY: %d", ret);
        return ret;
    }

    uint8_t buf[COMM_MSG_SET_BENEFICIARY_SIZE];
    size_t out_len;
    if (comm_msg_pack_set_beneficiary(buf, sizeof(buf),
                                      &s_keypair.public_key, &sig,
                                      target_key, beneficiary_key,
                                      forward_interval_min,
                                      &out_len) != 0) {
        ESP_LOGE(TAG, "Erreur pack SET_BENEFICIARY");
        return ESP_FAIL;
    }
    if (!transport_lora_send(buf, out_len, "SET_BENEFICIARY")) {
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "SET_BENEFICIARY envoye (interval=%u min)", forward_interval_min);
    return ESP_OK;
}
