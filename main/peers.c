/**
 * @file peers.c
 * @brief Implementation de la gestion des peers (voir peers.h).
 */

#include "peers.h"

#include <string.h>
#include <inttypes.h>

#include "esp_log.h"
#include "esp_timer.h"

#include "app_state.h"

static const char *TAG = "peers";

/*
 * [F-MN-011] Politique LRU sur la table des peers.
 *
 * `s_peer_last_seen[]` est un tableau parallèle à `s_peers[]` qui
 * stocke le timestamp (esp_timer_get_time, µs) de la dernière fois où
 * le peer a été observé (ajouté ou consulté). Quand la table est
 * pleine et qu'un nouveau peer arrive, on expulse l'entrée la plus
 * ancienne au lieu de rejeter silencieusement le nouveau.
 *
 * Stocké en local (pas dans `comm_peer_info_t`) pour éviter d'impacter
 * les autres composants qui utilisent cette struct.
 */
static uint64_t s_peer_last_seen[MAX_PEERS] = {0};

static inline void touch_peer_at(uint32_t idx)
{
    s_peer_last_seen[idx] = (uint64_t)esp_timer_get_time();
}

void add_peer(const comm_peer_info_t *peer)
{
    /* Verifier si le peer existe deja (idempotent + LRU touch). */
    for (uint32_t i = 0; i < s_peer_count; i++) {
        if (public_key_equal(&s_peers[i].public_key, &peer->public_key)) {
            memcpy(s_peers[i].mac_addr, peer->mac_addr,
                   sizeof(s_peers[i].mac_addr));

            /*
             * DISCOVER ne transporte pas d'alias et peut donc creer une
             * entree vide. Un ANNOUNCE ulterieur pour la meme pubkey doit
             * enrichir l'entree existante, sinon l'ecran Payer reste sur
             * "Sans nom" meme apres refresh.
             */
            if (peer->alias[0] != '\0') {
                strncpy(s_peers[i].alias, peer->alias,
                        sizeof(s_peers[i].alias) - 1);
                s_peers[i].alias[sizeof(s_peers[i].alias) - 1] = '\0';
            }

            touch_peer_at(i);
            ESP_LOGI(TAG, "Peer existant rafraichi (idx=%"PRIu32", alias=%s)",
                     i, s_peers[i].alias[0] ? s_peers[i].alias : "vide");
            return;
        }
    }

    if (s_peer_count < MAX_PEERS) {
        memcpy(&s_peers[s_peer_count], peer, sizeof(comm_peer_info_t));
        touch_peer_at(s_peer_count);
        s_peer_count++;
        ESP_LOGI(TAG, "Nouveau peer decouvert (%"PRIu32"/%d)",
                 s_peer_count, MAX_PEERS);
        return;
    }

    /*
     * [F-MN-011] Table pleine : expulser le peer le plus ancien (LRU)
     * pour laisser la place au nouveau. Sans ce mécanisme, les festivals
     * de plus de MAX_PEERS participants dégradaient silencieusement
     * vers LoRa-only pour les arrivants.
     */
    uint32_t oldest_idx = 0;
    uint64_t oldest_ts = s_peer_last_seen[0];
    for (uint32_t i = 1; i < MAX_PEERS; i++) {
        if (s_peer_last_seen[i] < oldest_ts) {
            oldest_ts = s_peer_last_seen[i];
            oldest_idx = i;
        }
    }
    ESP_LOGI(TAG, "Table peers pleine, expulsion LRU index=%lu (last_seen=%llu)",
             (unsigned long)oldest_idx, (unsigned long long)oldest_ts);
    memcpy(&s_peers[oldest_idx], peer, sizeof(comm_peer_info_t));
    touch_peer_at(oldest_idx);
}

const uint8_t *find_peer_mac(const public_key_t *key)
{
    for (uint32_t i = 0; i < s_peer_count; i++) {
        if (public_key_equal(&s_peers[i].public_key, key)) {
            /* [F-MN-011] Touch sur lookup : rafraîchit la position LRU. */
            touch_peer_at(i);
            return s_peers[i].mac_addr;
        }
    }
    return NULL;
}
