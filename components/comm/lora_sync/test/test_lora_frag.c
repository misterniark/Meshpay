/**
 * @file test_lora_frag.c
 * @brief Tests unitaires de la fragmentation/réassemblage LoRa.
 *
 * Teste les cas : split basique, réassemblage complet, réception
 * désordonnée, doublons, timeout, seq_id différent.
 */

#include "unity.h"
#include "comm/lora_frag.h"
#include <string.h>

/* Contexte de réassemblage partagé entre tests */
static lora_frag_ctx_t s_ctx;

void setUp(void)
{
    lora_frag_ctx_init(&s_ctx);
}

void tearDown(void)
{
}

/* ================================================================
 * Tests de fragmentation (split)
 * ================================================================ */

/**
 * Test : fragmenter un petit payload (< 251 octets) donne 1 fragment.
 */
TEST_CASE("frag_split_single", "[lora_frag]")
{
    uint8_t data[100];
    memset(data, 0xAA, 100);

    uint8_t packets[LORA_FRAG_MAX_FRAGMENTS][LORA_FRAG_PACKET_MAX];
    size_t packet_lens[LORA_FRAG_MAX_FRAGMENTS];
    uint8_t count = 0;

    TEST_ASSERT_EQUAL(0, lora_frag_split(data, 100, 42,
                                          packets, packet_lens, &count));
    TEST_ASSERT_EQUAL(1, count);

    /* Vérifier le header */
    TEST_ASSERT_EQUAL(COMM_MSG_LORA_FRAG, packets[0][0]); /* type */
    TEST_ASSERT_EQUAL(0, packets[0][1]);                   /* frag_index */
    TEST_ASSERT_EQUAL(1, packets[0][2]);                   /* total */
    TEST_ASSERT_EQUAL(42, packets[0][3]);                  /* seq_id */

    /* Vérifier la taille : header(4) + payload(100) */
    TEST_ASSERT_EQUAL(104, packet_lens[0]);
}

/**
 * Test : fragmenter 500 octets donne 2 fragments.
 */
TEST_CASE("frag_split_two", "[lora_frag]")
{
    uint8_t data[500];
    for (int i = 0; i < 500; i++) data[i] = (uint8_t)(i & 0xFF);

    uint8_t packets[LORA_FRAG_MAX_FRAGMENTS][LORA_FRAG_PACKET_MAX];
    size_t packet_lens[LORA_FRAG_MAX_FRAGMENTS];
    uint8_t count = 0;

    TEST_ASSERT_EQUAL(0, lora_frag_split(data, 500, 1,
                                          packets, packet_lens, &count));
    TEST_ASSERT_EQUAL(2, count);

    /* Premier fragment : 251 octets de payload */
    TEST_ASSERT_EQUAL(0, packets[0][1]);  /* index 0 */
    TEST_ASSERT_EQUAL(2, packets[0][2]);  /* total 2 */
    TEST_ASSERT_EQUAL(LORA_FRAG_HEADER_SIZE + LORA_FRAG_PAYLOAD_MAX,
                      packet_lens[0]);

    /* Second fragment : 500 - 251 = 249 octets de payload */
    TEST_ASSERT_EQUAL(1, packets[1][1]);  /* index 1 */
    TEST_ASSERT_EQUAL(2, packets[1][2]);  /* total 2 */
    TEST_ASSERT_EQUAL(LORA_FRAG_HEADER_SIZE + 249, packet_lens[1]);
}

/**
 * Test : données trop volumineuses (> 16 * 251) retourne erreur.
 */
TEST_CASE("frag_split_too_large", "[lora_frag]")
{
    /* 16 * 251 + 1 = 4017 octets — dépasse la limite */
    uint8_t data[4017];
    uint8_t packets[LORA_FRAG_MAX_FRAGMENTS][LORA_FRAG_PACKET_MAX];
    size_t packet_lens[LORA_FRAG_MAX_FRAGMENTS];
    uint8_t count = 0;

    TEST_ASSERT_EQUAL(-1, lora_frag_split(data, 4017, 0,
                                           packets, packet_lens, &count));
}

/* ================================================================
 * Tests de réassemblage
 * ================================================================ */

/**
 * Test : réassemblage dans l'ordre (2 fragments).
 */
TEST_CASE("frag_reassemble_in_order", "[lora_frag]")
{
    /* Données originales : 400 octets */
    uint8_t original[400];
    for (int i = 0; i < 400; i++) original[i] = (uint8_t)(i & 0xFF);

    /* Fragmenter */
    uint8_t packets[LORA_FRAG_MAX_FRAGMENTS][LORA_FRAG_PACKET_MAX];
    size_t packet_lens[LORA_FRAG_MAX_FRAGMENTS];
    uint8_t count;
    lora_frag_split(original, 400, 7, packets, packet_lens, &count);
    TEST_ASSERT_EQUAL(2, count);

    /* Réassembler dans l'ordre */
    bool done;
    done = lora_frag_receive(&s_ctx, 0, 2, 7,
                              &packets[0][LORA_FRAG_HEADER_SIZE],
                              packet_lens[0] - LORA_FRAG_HEADER_SIZE, 1000);
    TEST_ASSERT_FALSE(done);

    done = lora_frag_receive(&s_ctx, 1, 2, 7,
                              &packets[1][LORA_FRAG_HEADER_SIZE],
                              packet_lens[1] - LORA_FRAG_HEADER_SIZE, 1001);
    TEST_ASSERT_TRUE(done);

    /* Extraire et vérifier */
    uint8_t result[400];
    size_t result_len;
    TEST_ASSERT_EQUAL(0, lora_frag_get_result(&s_ctx, result, 400, &result_len));
    TEST_ASSERT_EQUAL(400, result_len);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(original, result, 400);
}

/**
 * Test : réassemblage dans le désordre.
 */
TEST_CASE("frag_reassemble_out_of_order", "[lora_frag]")
{
    uint8_t original[400];
    for (int i = 0; i < 400; i++) original[i] = (uint8_t)(i & 0xFF);

    uint8_t packets[LORA_FRAG_MAX_FRAGMENTS][LORA_FRAG_PACKET_MAX];
    size_t packet_lens[LORA_FRAG_MAX_FRAGMENTS];
    uint8_t count;
    lora_frag_split(original, 400, 5, packets, packet_lens, &count);

    /* Envoyer fragment 1 avant fragment 0 */
    bool done;
    done = lora_frag_receive(&s_ctx, 1, 2, 5,
                              &packets[1][LORA_FRAG_HEADER_SIZE],
                              packet_lens[1] - LORA_FRAG_HEADER_SIZE, 1000);
    TEST_ASSERT_FALSE(done);

    done = lora_frag_receive(&s_ctx, 0, 2, 5,
                              &packets[0][LORA_FRAG_HEADER_SIZE],
                              packet_lens[0] - LORA_FRAG_HEADER_SIZE, 1001);
    TEST_ASSERT_TRUE(done);

    uint8_t result[400];
    size_t result_len;
    TEST_ASSERT_EQUAL(0, lora_frag_get_result(&s_ctx, result, 400, &result_len));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(original, result, 400);
}

/**
 * Test : doublons ignorés sans erreur.
 */
TEST_CASE("frag_duplicate_fragment", "[lora_frag]")
{
    uint8_t payload[100];
    memset(payload, 0xBB, 100);

    /* Envoyer le même fragment deux fois */
    bool done;
    done = lora_frag_receive(&s_ctx, 0, 2, 10, payload, 100, 1000);
    TEST_ASSERT_FALSE(done);

    /* Doublon — ne doit pas crasher ni marquer comme complet */
    done = lora_frag_receive(&s_ctx, 0, 2, 10, payload, 100, 1001);
    TEST_ASSERT_FALSE(done);

    /* Envoyer le fragment manquant */
    done = lora_frag_receive(&s_ctx, 1, 2, 10, payload, 100, 1002);
    TEST_ASSERT_TRUE(done);
}

/**
 * Test : nouveau seq_id abandonne l'ancien réassemblage.
 */
TEST_CASE("frag_new_seq_id_resets", "[lora_frag]")
{
    uint8_t payload[50];
    memset(payload, 0xCC, 50);

    /* Commencer un réassemblage avec seq_id=1 */
    lora_frag_receive(&s_ctx, 0, 3, 1, payload, 50, 1000);
    TEST_ASSERT_TRUE(s_ctx.active);
    TEST_ASSERT_EQUAL(1, s_ctx.seq_id);

    /* Arrivée d'un fragment avec seq_id=2 → reset */
    lora_frag_receive(&s_ctx, 0, 2, 2, payload, 50, 2000);
    TEST_ASSERT_EQUAL(2, s_ctx.seq_id);
    TEST_ASSERT_EQUAL(2, s_ctx.total_fragments);
}

/**
 * Test : timeout expire le réassemblage.
 */
TEST_CASE("frag_timeout_expire", "[lora_frag]")
{
    uint8_t payload[50];
    memset(payload, 0xDD, 50);

    /* Envoyer un fragment */
    lora_frag_receive(&s_ctx, 0, 2, 3, payload, 50, 1000);
    TEST_ASSERT_TRUE(s_ctx.active);

    /* Pas encore timeout (9 secondes) */
    lora_frag_expire(&s_ctx, 1000 + 9000);
    TEST_ASSERT_TRUE(s_ctx.active);

    /* Timeout (10 secondes) */
    lora_frag_expire(&s_ctx, 1000 + 10000);
    TEST_ASSERT_FALSE(s_ctx.active);
}

/**
 * Test : get_result échoue si réassemblage incomplet.
 */
TEST_CASE("frag_get_result_incomplete", "[lora_frag]")
{
    uint8_t payload[50];
    memset(payload, 0xEE, 50);

    lora_frag_receive(&s_ctx, 0, 2, 4, payload, 50, 1000);

    uint8_t out[200];
    size_t out_len;
    TEST_ASSERT_EQUAL(-1, lora_frag_get_result(&s_ctx, out, 200, &out_len));
}
