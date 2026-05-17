/**
 * @file test_comm_msg_time_sync.c
 * @brief Tests unitaires du pack/unpack du message LORA_TIME_SYNC signé.
 *
 * Vérifie le round-trip, le format wire avec signature Ed25519,
 * et les cas d'erreur (buffer trop court, type incorrect, NULL).
 */

#include "unity.h"
#include "comm/comm_msg.h"
#include <string.h>


/* ================================================================
 * Helpers
 * ================================================================ */

static void fill_pubkey(public_key_t *key, uint8_t pattern)
{
    memset(key->bytes, pattern, CRYPTO_PUBLIC_KEY_SIZE);
}

static void fill_signature(signature_t *sig, uint8_t pattern)
{
    memset(sig->bytes, pattern, CRYPTO_SIGNATURE_SIZE);
}

/* ================================================================
 * Tests TIME_SYNC
 * ================================================================ */

/**
 * Test : pack TIME_SYNC — vérifie le format wire signé.
 * Format : [0x12][pubkey:32][sig:64][timestamp:8 BE][lamport:8 BE]
 */
TEST_CASE("time_sync_pack_basic", "[comm_msg]")
{
    uint8_t buf[COMM_MSG_LORA_MAX];
    size_t out_len = 0;

    public_key_t key;
    fill_pubkey(&key, 0xAA);

    signature_t sig;
    fill_signature(&sig, 0x55);

    uint64_t timestamp = 0x0102030405060708ULL;
    uint64_t lamport   = 0x1112131415161718ULL;

    int ret = comm_msg_pack_time_sync(buf, sizeof(buf), &key, &sig,
                                       timestamp, lamport, &out_len);
    TEST_ASSERT_EQUAL(0, ret);
    TEST_ASSERT_EQUAL(COMM_MSG_TIME_SYNC_SIZE, out_len);
    TEST_ASSERT_EQUAL(113, out_len);

    /* Vérifier le type */
    TEST_ASSERT_EQUAL(COMM_MSG_LORA_TIME_SYNC, buf[0]);

    /* Vérifier la pubkey (offset 1..32) */
    TEST_ASSERT_EQUAL_MEMORY(key.bytes, &buf[1], CRYPTO_PUBLIC_KEY_SIZE);

    /* Vérifier la signature (offset 33..96) */
    TEST_ASSERT_EQUAL_MEMORY(sig.bytes, &buf[33], CRYPTO_SIGNATURE_SIZE);

    /* Vérifier le timestamp en big-endian (offset 97..104) */
    TEST_ASSERT_EQUAL(0x01, buf[97]);
    TEST_ASSERT_EQUAL(0x08, buf[104]);

    /* Vérifier le lamport en big-endian (offset 105..112) */
    TEST_ASSERT_EQUAL(0x11, buf[105]);
    TEST_ASSERT_EQUAL(0x18, buf[112]);
}

/**
 * Test : round-trip pack → unpack TIME_SYNC signé.
 */
TEST_CASE("time_sync_roundtrip", "[comm_msg]")
{
    uint8_t buf[COMM_MSG_LORA_MAX];
    size_t out_len = 0;

    public_key_t key;
    fill_pubkey(&key, 0xBB);

    signature_t sig;
    fill_signature(&sig, 0x77);

    uint64_t timestamp = 1700000000000ULL;
    uint64_t lamport   = 42;

    TEST_ASSERT_EQUAL(0, comm_msg_pack_time_sync(buf, sizeof(buf), &key, &sig,
                                                  timestamp, lamport, &out_len));

    comm_msg_time_sync_t msg;
    TEST_ASSERT_EQUAL(0, comm_msg_unpack_time_sync(buf, out_len, &msg));

    TEST_ASSERT_EQUAL_MEMORY(key.bytes, msg.master_key.bytes, CRYPTO_PUBLIC_KEY_SIZE);
    TEST_ASSERT_EQUAL_MEMORY(sig.bytes, msg.signature.bytes, CRYPTO_SIGNATURE_SIZE);
    TEST_ASSERT_EQUAL(timestamp, msg.master_timestamp);
    TEST_ASSERT_EQUAL(lamport, msg.master_lamport);
}

/**
 * Test : pack TIME_SYNC avec buffer trop petit → erreur.
 */
TEST_CASE("time_sync_pack_buffer_too_small", "[comm_msg]")
{
    uint8_t buf[50]; /* Trop petit pour 113 octets */
    size_t out_len = 0;
    public_key_t key;
    fill_pubkey(&key, 0x11);
    signature_t sig;
    fill_signature(&sig, 0x22);

    TEST_ASSERT_EQUAL(-1, comm_msg_pack_time_sync(buf, sizeof(buf), &key, &sig,
                                                   0, 0, &out_len));
}

/**
 * Test : unpack TIME_SYNC avec mauvais type → erreur.
 */
TEST_CASE("time_sync_unpack_wrong_type", "[comm_msg]")
{
    uint8_t buf[COMM_MSG_TIME_SYNC_SIZE];
    memset(buf, 0, sizeof(buf));
    buf[0] = COMM_MSG_LORA_BROADCAST; /* Mauvais type */

    comm_msg_time_sync_t msg;
    TEST_ASSERT_EQUAL(-1, comm_msg_unpack_time_sync(buf, sizeof(buf), &msg));
}

/**
 * Test : pack TIME_SYNC avec params NULL → erreur.
 */
TEST_CASE("time_sync_pack_null_params", "[comm_msg]")
{
    uint8_t buf[COMM_MSG_LORA_MAX];
    size_t out_len = 0;
    public_key_t key = {0};
    signature_t sig = {0};

    TEST_ASSERT_EQUAL(-1, comm_msg_pack_time_sync(NULL, sizeof(buf), &key, &sig, 0, 0, &out_len));
    TEST_ASSERT_EQUAL(-1, comm_msg_pack_time_sync(buf, sizeof(buf), NULL, &sig, 0, 0, &out_len));
    TEST_ASSERT_EQUAL(-1, comm_msg_pack_time_sync(buf, sizeof(buf), &key, NULL, 0, 0, &out_len));
    TEST_ASSERT_EQUAL(-1, comm_msg_pack_time_sync(buf, sizeof(buf), &key, &sig, 0, 0, NULL));
}

/**
 * Test : unpack TIME_SYNC avec buffer trop court → erreur.
 */
TEST_CASE("time_sync_unpack_buffer_too_short", "[comm_msg]")
{
    uint8_t buf[50];
    buf[0] = COMM_MSG_LORA_TIME_SYNC;

    comm_msg_time_sync_t msg;
    TEST_ASSERT_EQUAL(-1, comm_msg_unpack_time_sync(buf, sizeof(buf), &msg));
}

/**
 * Test : comm_msg_get_type reconnaît LORA_TIME_SYNC.
 */
TEST_CASE("time_sync_get_type", "[comm_msg]")
{
    uint8_t buf[1] = { COMM_MSG_LORA_TIME_SYNC };
    comm_msg_type_t type;

    TEST_ASSERT_EQUAL(0, comm_msg_get_type(buf, 1, &type));
    TEST_ASSERT_EQUAL(COMM_MSG_LORA_TIME_SYNC, type);
}
