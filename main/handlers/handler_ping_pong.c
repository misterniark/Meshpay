/**
 * @file handlers/handler_ping_pong.c
 * @brief Reception d'un PING maitre et collecte des PONGs.
 *
 * PING : initie par le maitre, relaye par les devices (anti-boucle via
 *        cache de seen ping_ids). Chaque device repond par un PONG signe.
 * PONG : collecte par le maitre uniquement (ping_id correspondant a la
 *        session active).
 */

#include "handlers.h"

#include <inttypes.h>
#include <string.h>

#include "esp_log.h"
#include "esp_random.h"

#include "app_state.h"
#include "comm/comm_msg.h"
#include "crypto/crypto_sign.h"
#include "transport/transport_lora.h"

static const char *TAG = "h_ping";

static bool ping_already_seen(const public_key_t *master_key, uint16_t ping_id)
{
    uint32_t limit = (s_seen_ping_count < MAX_SEEN_PINGS)
                         ? s_seen_ping_count
                         : MAX_SEEN_PINGS;
    for (uint32_t i = 0; i < limit; i++) {
        if (s_seen_pings[i].ping_id == ping_id &&
            public_key_equal(&s_seen_pings[i].master_key, master_key)) {
            return true;
        }
    }
    return false;
}

static void ping_mark_seen(const public_key_t *master_key, uint16_t ping_id)
{
    uint32_t idx = s_seen_ping_count % MAX_SEEN_PINGS;
    memcpy(&s_seen_pings[idx].master_key, master_key, sizeof(public_key_t));
    s_seen_pings[idx].ping_id = ping_id;
    s_seen_ping_count++;
}

void ping_mark_seen_public(const public_key_t *master_key, uint16_t ping_id)
{
    ping_mark_seen(master_key, ping_id);
}

void handle_ping_received(const comm_event_t *evt)
{
    const comm_msg_ping_t *ping = &evt->data.ping;

    /* Verifier emetteur dans mint_authorities (PINGs reserves aux maitres). */
    bool is_authorized = false;
    for (uint32_t i = 0; i < s_currency.mint_authority_count; i++) {
        if (public_key_equal(&ping->master_key,
                             &s_currency.mint_authorities[i])) {
            is_authorized = true;
            break;
        }
    }
    if (!is_authorized) {
        ESP_LOGW(TAG, "PING rejete : emetteur non autorise");
        return;
    }

    if (ping_already_seen(&ping->master_key, ping->ping_id)) {
        ESP_LOGD(TAG, "Ping deja vu (id=%u), ignore", ping->ping_id);
        return;
    }
    /* Ignorer notre propre PING (cas maitre). */
    if (public_key_equal(&ping->master_key, &s_keypair.public_key)) {
        return;
    }

    ping_mark_seen(&ping->master_key, ping->ping_id);
    ESP_LOGI(TAG, "Ping maitre recu (id=%u), envoi PONG", ping->ping_id);

    /*
     * Preparer le PONG signe (envoi differe par transport_lora_pump).
     *
     * [I4-fix] Signature sur [ping_id:2 BE][alias_len:1][alias:N] pour
     * empecher l'usurpation d'identite.
     */
    {
        uint8_t sign_buf[2 + 1 + COMM_MSG_ALIAS_MAX];
        size_t sign_len = 0;
        sign_buf[sign_len++] = (uint8_t)(ping->ping_id >> 8);
        sign_buf[sign_len++] = (uint8_t)(ping->ping_id);
        sign_buf[sign_len++] = s_device_alias_len;
        memcpy(&sign_buf[sign_len], s_device_alias, s_device_alias_len);
        sign_len += s_device_alias_len;

        signature_t pong_sig;
        if (crypto_sign(sign_buf, sign_len, &s_keypair, &pong_sig) != ESP_OK) {
            ESP_LOGE(TAG, "Erreur signature PONG");
        } else {
            uint8_t pong_buf[COMM_MSG_LORA_MAX];
            size_t pong_len;
            if (comm_msg_pack_pong(pong_buf, sizeof(pong_buf),
                                   &s_keypair.public_key,
                                   &pong_sig,
                                   ping->ping_id,
                                   s_device_alias, s_device_alias_len,
                                   &pong_len) == 0) {
                uint32_t delay_ms = 1000 + (esp_random() % 4001);
                transport_lora_queue_pong_delayed(pong_buf, pong_len, delay_ms);
                ESP_LOGI(TAG, "PONG signe prepare (envoi differe dans %lums)",
                         (unsigned long)delay_ms);
            }
        }
    }

    /* Relay PING (signature originale du maitre reutilisee). */
    {
        uint8_t relay_buf[COMM_MSG_PING_SIZE];
        size_t  relay_len = 0;
        if (comm_msg_pack_ping(relay_buf, sizeof(relay_buf),
                               &ping->master_key, &ping->signature,
                               ping->ping_id, &relay_len) == 0) {
            transport_lora_queue_relay_ping(relay_buf, relay_len);
        }
    }
}

void handle_pong_received(const comm_event_t *evt)
{
    const comm_msg_pong_t *pong = &evt->data.pong;

    if (!s_ping_active || pong->ping_id != s_current_ping_id) {
        ESP_LOGD(TAG, "PONG ignore (session inactive ou id mismatch)");
        return;
    }
    if (public_key_equal(&pong->device_key, &s_keypair.public_key)) {
        return;
    }

    for (uint32_t i = 0; i < s_ping_result_count; i++) {
        if (public_key_equal(&s_ping_results[i].key, &pong->device_key)) {
            return;
        }
    }

    if (s_ping_result_count >= MAX_PING_RESULTS) {
        ESP_LOGW(TAG, "Table ping pleine, PONG ignore");
        return;
    }

    memcpy(&s_ping_results[s_ping_result_count].key,
           &pong->device_key, sizeof(public_key_t));
    memcpy(s_ping_results[s_ping_result_count].alias,
           pong->alias, pong->alias_len);
    s_ping_results[s_ping_result_count].alias[pong->alias_len] = '\0';
    s_ping_results[s_ping_result_count].alias_len = pong->alias_len;
    s_ping_result_count++;

    ESP_LOGI(TAG, "PONG recu : \"%s\" (%"PRIu32"/%d devices)",
             pong->alias, s_ping_result_count, MAX_PING_RESULTS);
}
