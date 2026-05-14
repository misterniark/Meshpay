/**
 * @file test_lora_tx_packetize.c
 * @brief Tests unitaires de lora_tx_packetize() — emballage d'une
 *        transaction confirmée en paquet(s) LoRa, direct ou fragmenté.
 *
 * Ne définit volontairement PAS setUp()/tearDown() : test_lora_frag.c
 * fournit déjà des versions weak partagées par le composant.
 */

#include "unity.h"
#include "comm/lora_tx_packetize.h"
#include "comm/comm_msg.h"   /* COMM_MSG_LORA_TX, COMM_MSG_LORA_FRAG, COMM_MSG_LORA_MAX */
#include "comm/lora_frag.h"
#include "transaction/tx_serialize.h"
#include "transaction/tx_types.h"
#include "esp_err.h"
#include <string.h>

/*
 * Petite TX : 1 parent, tous les champs à zéro. Son CBOR (~225 octets)
 * tient dans un seul paquet LoRa.
 */
static void build_small_tx(transaction_t *tx)
{
    memset(tx, 0, sizeof(*tx));
    tx->type         = TX_TYPE_TRANSFER;
    tx->parent_count = 1;
    tx->status       = TX_STATUS_CONFIRMED;
}

/*
 * Grosse TX : 2 parents, tous les champs binaires remplis de 0xFF et les
 * entiers à leur max. Reproduit le pire cas réel (TRANSFER à 2 parents),
 * dont le CBOR (~282 octets) dépasse COMM_MSG_LORA_MAX et doit donc être
 * fragmenté. Le memset(0) initial garde les octets de padding de la
 * struct déterministes (à 0) pour permettre un memcmp round-trip fiable.
 */
static void build_large_tx(transaction_t *tx)
{
    memset(tx, 0, sizeof(*tx));
    tx->type         = TX_TYPE_TRANSFER;
    tx->status       = TX_STATUS_CONFIRMED;
    tx->parent_count = 2;
    tx->amount       = 0xFFFFFFFFu;
    tx->currency_id  = 0xFFFFFFFFu;
    tx->fee          = 0xFFFFFFFFu;
    tx->seq          = 0xFFFFFFFFu;
    tx->timestamp    = 0xFFFFFFFFFFFFFFFFull;
    memset(&tx->id,        0xFF, sizeof(tx->id));
    memset(&tx->from,      0xFF, sizeof(tx->from));
    memset(&tx->to,        0xFF, sizeof(tx->to));
    memset(&tx->parents,   0xFF, sizeof(tx->parents));
    memset(&tx->signature, 0xFF, sizeof(tx->signature));
}

TEST_CASE("packetize_small_tx_single_packet", "[lora_tx_packetize]")
{
    transaction_t tx;
    build_small_tx(&tx);

    uint8_t packets[LORA_FRAG_MAX_FRAGMENTS][LORA_FRAG_PACKET_MAX];
    size_t  packet_lens[LORA_FRAG_MAX_FRAGMENTS];
    uint8_t count = 0;

    TEST_ASSERT_EQUAL(0, lora_tx_packetize(&tx, 7, packets, packet_lens, &count));
    TEST_ASSERT_EQUAL(1, count);
    TEST_ASSERT_EQUAL(COMM_MSG_LORA_TX, packets[0][0]);
    TEST_ASSERT_LESS_OR_EQUAL(COMM_MSG_LORA_MAX, packet_lens[0]);
}

TEST_CASE("packetize_large_tx_fragments", "[lora_tx_packetize]")
{
    transaction_t tx;
    build_large_tx(&tx);

    uint8_t packets[LORA_FRAG_MAX_FRAGMENTS][LORA_FRAG_PACKET_MAX];
    size_t  packet_lens[LORA_FRAG_MAX_FRAGMENTS];
    uint8_t count = 0;

    TEST_ASSERT_EQUAL(0, lora_tx_packetize(&tx, 42, packets, packet_lens, &count));
    /*
     * Une TX TRANSFER à 2 parents dépasse 254 octets de CBOR. Si cette
     * assertion échoue, le pire cas tient en fait dans un seul paquet et
     * le bug d'origine n'était pas atteignable — information utile en soi.
     */
    TEST_ASSERT_GREATER_OR_EQUAL(2, count);
    for (uint8_t i = 0; i < count; i++) {
        TEST_ASSERT_EQUAL(COMM_MSG_LORA_FRAG, packets[i][0]); /* type */
        TEST_ASSERT_EQUAL(i, packets[i][1]);                  /* index */
        TEST_ASSERT_EQUAL(count, packets[i][2]);              /* total */
        TEST_ASSERT_EQUAL(42, packets[i][3]);                 /* seq_id */
        TEST_ASSERT_GREATER_OR_EQUAL(LORA_FRAG_HEADER_SIZE, packet_lens[i]);
    }
}

TEST_CASE("packetize_large_tx_roundtrip", "[lora_tx_packetize]")
{
    transaction_t original;
    build_large_tx(&original);

    uint8_t packets[LORA_FRAG_MAX_FRAGMENTS][LORA_FRAG_PACKET_MAX];
    size_t  packet_lens[LORA_FRAG_MAX_FRAGMENTS];
    uint8_t count = 0;
    TEST_ASSERT_EQUAL(0, lora_tx_packetize(&original, 99, packets, packet_lens, &count));
    TEST_ASSERT_GREATER_OR_EQUAL(2, count);

    /* Réassembler les fragments comme le ferait le récepteur LoRa. */
    lora_frag_ctx_t ctx;
    lora_frag_ctx_init(&ctx);
    bool complete = false;
    for (uint8_t i = 0; i < count; i++) {
        complete = lora_frag_receive(&ctx,
                                     packets[i][1], packets[i][2], packets[i][3],
                                     &packets[i][LORA_FRAG_HEADER_SIZE],
                                     packet_lens[i] - LORA_FRAG_HEADER_SIZE,
                                     0 /* current_time */);
    }
    TEST_ASSERT_TRUE(complete);

    uint8_t result[LORA_FRAG_MAX_FRAGMENTS * LORA_FRAG_PAYLOAD_MAX];
    size_t  result_len = 0;
    TEST_ASSERT_EQUAL(0, lora_frag_get_result(&ctx, result, sizeof(result), &result_len));

    /*
     * Le buffer réassemblé est le CBOR nu : tx_deserialize() doit
     * reconstruire la TX à l'identique. memset(0) garantit que les octets
     * de padding de `restored` matchent ceux d'`original`.
     */
    transaction_t restored;
    memset(&restored, 0, sizeof(restored));
    TEST_ASSERT_EQUAL(ESP_OK, tx_deserialize(result, result_len, &restored));
    TEST_ASSERT_EQUAL_MEMORY(&original, &restored, sizeof(transaction_t));
}

TEST_CASE("packetize_null_args", "[lora_tx_packetize]")
{
    uint8_t packets[LORA_FRAG_MAX_FRAGMENTS][LORA_FRAG_PACKET_MAX];
    size_t  packet_lens[LORA_FRAG_MAX_FRAGMENTS];
    uint8_t count = 0;
    transaction_t tx;
    build_small_tx(&tx);

    TEST_ASSERT_EQUAL(-1, lora_tx_packetize(NULL, 0, packets, packet_lens, &count));
    TEST_ASSERT_EQUAL(-1, lora_tx_packetize(&tx, 0, NULL, packet_lens, &count));
    TEST_ASSERT_EQUAL(-1, lora_tx_packetize(&tx, 0, packets, NULL, &count));
    TEST_ASSERT_EQUAL(-1, lora_tx_packetize(&tx, 0, packets, packet_lens, NULL));
}
