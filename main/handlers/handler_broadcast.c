/**
 * @file handlers/handler_broadcast.c
 * @brief Reception d'un broadcast texte maitre via LoRa.
 *
 * Pipeline :
 * 1. Anti-boucle relay (cache des signatures vues, buffer circulaire)
 * 2. Verifier emetteur dans mint_authorities
 * 3. Verifier signature Ed25519
 * 4. Stocker pour l'UI + filer le relay LoRa (no-op sur stub)
 */

#include "handlers.h"

#include <string.h>

#include "esp_log.h"

#include "app_state.h"
#include "comm/comm_msg.h"
#include "crypto/crypto_sign.h"
#include "transport/transport_lora.h"

static const char *TAG = "h_bcast";

static bool broadcast_already_seen(const signature_t *sig)
{
    uint32_t limit = (s_seen_bcast_count < MAX_SEEN_BROADCASTS)
                         ? s_seen_bcast_count
                         : MAX_SEEN_BROADCASTS;
    for (uint32_t i = 0; i < limit; i++) {
        if (memcmp(s_seen_bcast[i].bytes, sig->bytes,
                   CRYPTO_SIGNATURE_SIZE) == 0) {
            return true;
        }
    }
    return false;
}

static void broadcast_mark_seen(const signature_t *sig)
{
    uint32_t idx = s_seen_bcast_count % MAX_SEEN_BROADCASTS;
    memcpy(&s_seen_bcast[idx], sig, sizeof(signature_t));
    s_seen_bcast_count++;
}

void handle_broadcast_received(const comm_event_t *evt)
{
    const comm_msg_broadcast_t *bcast = &evt->data.broadcast;

    if (broadcast_already_seen(&bcast->signature)) {
        ESP_LOGD(TAG, "Broadcast deja vu, ignore");
        return;
    }

    bool is_master = false;
    for (uint32_t i = 0; i < s_currency.mint_authority_count; i++) {
        if (public_key_equal(&bcast->sender_key,
                             &s_currency.mint_authorities[i])) {
            is_master = true;
            break;
        }
    }
    if (!is_master) {
        ESP_LOGW(TAG, "Broadcast ignore : emetteur non autorise");
        return;
    }

    /* Signature couvre [text_len:1][text:N]. */
    uint8_t signed_data[1 + COMM_MSG_BROADCAST_TEXT_MAX];
    signed_data[0] = bcast->text_len;
    memcpy(&signed_data[1], bcast->text, bcast->text_len);
    size_t signed_len = 1 + bcast->text_len;

    esp_err_t ret = crypto_verify(signed_data, signed_len,
                                  &bcast->sender_key, &bcast->signature);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Broadcast ignore : signature invalide");
        return;
    }

    /* Mark vu AVANT relay (anti-boucle si on recoit notre propre relay). */
    broadcast_mark_seen(&bcast->signature);

    memcpy(&s_pending_broadcast, bcast, sizeof(comm_msg_broadcast_t));
    s_broadcast_pending = true;

    ESP_LOGI(TAG, "Broadcast maitre accepte (%u chars): \"%.*s\"",
             bcast->text_len, bcast->text_len, bcast->text);

    /* Filer le relay LoRa (no-op sur cibles sans LoRa). */
    {
        uint8_t relay_buf[COMM_MSG_LORA_MAX];
        size_t  relay_len = 0;
        if (comm_msg_pack_broadcast(relay_buf, sizeof(relay_buf),
                                    &bcast->sender_key, &bcast->signature,
                                    bcast->text, bcast->text_len,
                                    &relay_len) == 0) {
            transport_lora_queue_relay_broadcast(relay_buf, relay_len);
        }
    }
}
