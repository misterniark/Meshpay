/**
 * @file peers.c
 * @brief Implementation de la gestion des peers (voir peers.h).
 */

#include "peers.h"

#include <string.h>
#include <inttypes.h>

#include "esp_log.h"

#include "app_state.h"

static const char *TAG = "peers";

void add_peer(const comm_peer_info_t *peer)
{
    /* Verifier si le peer existe deja (idempotent). */
    for (uint32_t i = 0; i < s_peer_count; i++) {
        if (public_key_equal(&s_peers[i].public_key, &peer->public_key)) {
            return;
        }
    }

    if (s_peer_count >= MAX_PEERS) {
        ESP_LOGW(TAG, "Table des peers pleine, ignore nouveau peer");
        return;
    }

    memcpy(&s_peers[s_peer_count], peer, sizeof(comm_peer_info_t));
    s_peer_count++;
    ESP_LOGI(TAG, "Nouveau peer decouvert (%"PRIu32"/%d)", s_peer_count, MAX_PEERS);
}

const uint8_t *find_peer_mac(const public_key_t *key)
{
    for (uint32_t i = 0; i < s_peer_count; i++) {
        if (public_key_equal(&s_peers[i].public_key, key)) {
            return s_peers[i].mac_addr;
        }
    }
    return NULL;
}
