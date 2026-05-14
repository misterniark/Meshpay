/**
 * @file lora_tx_packetize.c
 * @brief Emballage d'une transaction confirmée en paquet(s) LoRa.
 */

#include "comm/lora_tx_packetize.h"
#include "comm/comm_msg.h"            /* COMM_MSG_LORA_TX, COMM_MSG_LORA_MAX */
#include "comm/lora_frag.h"           /* lora_frag_split() */
#include "transaction/tx_serialize.h" /* tx_serialize_full(), TX_CBOR_MAX_SIZE */
#include <string.h>

int lora_tx_packetize(const transaction_t *tx,
                      uint8_t seq_id,
                      uint8_t packets[][LORA_FRAG_PACKET_MAX],
                      size_t  *packet_lens,
                      uint8_t *packet_count)
{
    if (!tx || !packets || !packet_lens || !packet_count) {
        return -1;
    }

    /*
     * Sérialiser la TX complète en CBOR dans un buffer dimensionné au pire
     * cas (TX_CBOR_MAX_SIZE = 320 octets) : tx_serialize_full() ne peut
     * donc pas échouer faute de place pour une TX valide.
     */
    uint8_t cbor[TX_CBOR_MAX_SIZE];
    size_t  cbor_len = 0;
    if (tx_serialize_full(tx, cbor, sizeof(cbor), &cbor_len) != ESP_OK) {
        return -1;
    }

    /*
     * Cas direct : [type:1][cbor...] tient dans un paquet LoRa. Le byte de
     * type est nécessaire ici car le récepteur appelle
     * comm_msg_unpack_lora_tx() sur le paquet entier.
     */
    if (1 + cbor_len <= COMM_MSG_LORA_MAX) {
        packets[0][0] = COMM_MSG_LORA_TX;
        memcpy(&packets[0][1], cbor, cbor_len);
        packet_lens[0] = 1 + cbor_len;
        *packet_count  = 1;
        return 0;
    }

    /*
     * Cas fragmenté : on fragmente le CBOR *nu*, sans byte de type. Le
     * récepteur réassemble puis appelle tx_deserialize() directement sur
     * le buffer concaténé — il n'attend donc aucun préfixe.
     */
    return lora_frag_split(cbor, cbor_len, seq_id,
                           packets, packet_lens, packet_count);
}
