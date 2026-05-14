/**
 * @file lora_tx_packetize.h
 * @brief Emballage d'une transaction confirmée en paquet(s) LoRa.
 *
 * Module pur : aucune dépendance FreeRTOS ni HAL, testable en natif.
 * Pont entre la (dé)sérialisation CBOR (`transaction`) et la
 * fragmentation (`lora_frag`).
 */

#ifndef LORA_TX_PACKETIZE_H
#define LORA_TX_PACKETIZE_H

#include "transaction/tx_types.h"  /* transaction_t */
#include "comm/lora_frag.h"        /* LORA_FRAG_PACKET_MAX, LORA_FRAG_MAX_FRAGMENTS */
#include <stdint.h>
#include <stddef.h>

/**
 * Transforme une transaction en un ou plusieurs paquets LoRa prêts à émettre.
 *
 * - Si la TX sérialisée tient dans un paquet LoRa (1 + cbor <= COMM_MSG_LORA_MAX),
 *   produit 1 paquet direct : [COMM_MSG_LORA_TX][cbor...].
 * - Sinon, fragmente le CBOR *nu* via lora_frag_split() : N paquets
 *   [COMM_MSG_LORA_FRAG][index][total][seq_id][chunk...].
 *
 * L'asymétrie (byte de type présent dans le cas direct, absent des
 * fragments) est imposée par le récepteur : voir lora_sync.c, qui appelle
 * comm_msg_unpack_lora_tx() sur le paquet direct mais tx_deserialize()
 * sur le buffer réassemblé.
 *
 * @param tx            Transaction à émettre
 * @param seq_id        Identifiant de séquence de fragmentation (0-255),
 *                      utilisé uniquement si la TX est fragmentée
 * @param packets       [out] Tableau de paquets (LORA_FRAG_MAX_FRAGMENTS lignes)
 * @param packet_lens   [out] Taille réelle de chaque paquet produit
 * @param packet_count  [out] Nombre de paquets produits (toujours >= 1 en cas de succès)
 * @return 0 en cas de succès,
 *         -1 si paramètre NULL, échec de sérialisation, ou TX trop
 *         volumineuse pour LORA_FRAG_MAX_FRAGMENTS fragments
 */
int lora_tx_packetize(const transaction_t *tx,
                      uint8_t seq_id,
                      uint8_t packets[][LORA_FRAG_PACKET_MAX],
                      size_t  *packet_lens,
                      uint8_t *packet_count);

#endif /* LORA_TX_PACKETIZE_H */
