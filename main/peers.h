/**
 * @file peers.h
 * @brief Table des peers ESP-NOW decouverts.
 *
 * La storage de la table est dans app_state (s_peers, s_peer_count).
 * Ce module fournit les operations qui la manipulent :
 *   - add_peer       : ajoute un peer (idempotent, capacite MAX_PEERS)
 *   - find_peer_mac  : recherche l'adresse MAC par cle publique
 *
 * Les appelants (handlers, ops) doivent etre sous s_state_mutex.
 */

#ifndef MESHPAY_PEERS_H
#define MESHPAY_PEERS_H

#include <stdint.h>
#include "crypto/crypto_types.h"
#include "comm/comm_event.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Ajoute un peer a la table (ignore les doublons).
 *
 * Si la table est pleine (MAX_PEERS atteint), loggue un WARN et ignore.
 */
void add_peer(const comm_peer_info_t *peer);

/**
 * @brief Recherche l'adresse MAC d'un peer par sa cle publique.
 *
 * @return Pointeur vers le mac_addr du peer (interne a s_peers), ou NULL.
 *         Valide tant que la table n'est pas modifiee (donc sous mutex).
 */
const uint8_t *find_peer_mac(const public_key_t *key);

#ifdef __cplusplus
}
#endif

#endif /* MESHPAY_PEERS_H */
