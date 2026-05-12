/**
 * @file test_comm_msg_ping_pong.c
 * @brief Tests unitaires du pack/unpack des messages LORA_PING et LORA_PONG.
 *
 * Vérifie le round-trip, les cas limites (alias max, alias vide),
 * et les cas d'erreur (buffer trop court, type incorrect, NULL).
 */

#include "unity.h"
#include "comm/comm_msg.h"
#include <string.h>

void setUp(void) {}
void tearDown(void) {}

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
 * Tests PING
 * ================================================================ */

/**
 * Test : pack PING — vérifie le format wire signé.
 * Format : [0x14][pubkey:32][sig:64][ping_id:2 BE]
 */
TEST_CASE("ping_pack_basic", "[comm_msg]")
{
    uint8_t buf[COMM_MSG_LORA_MAX];
    size_t out_len = 0;

    public_key_t key;
    fill_pubkey(&key, 0xAA);

    signature_t sig;
    fill_signature(&sig, 0x55);

    int ret = comm_msg_pack_ping(buf, sizeof(buf), &key, &sig, 0x1234, &out_len);
    TEST_ASSERT_EQUAL(0, ret);
    TEST_ASSERT_EQUAL(COMM_MSG_PING_SIZE, out_len);

    /* Vérifier le type */
    TEST_ASSERT_EQUAL(COMM_MSG_LORA_PING, buf[0]);

    /* Vérifier la pubkey */
    TEST_ASSERT_EQUAL_MEMORY(key.bytes, &buf[1], CRYPTO_PUBLIC_KEY_SIZE);

    /* Vérifier la signature */
    TEST_ASSERT_EQUAL_MEMORY(sig.bytes, &buf[33], CRYPTO_SIGNATURE_SIZE);

    /* Vérifier ping_id en big-endian (après pubkey + sig) */
    TEST_ASSERT_EQUAL(0x12, buf[97]);
    TEST_ASSERT_EQUAL(0x34, buf[98]);
}

/**
 * Test : round-trip pack → unpack PING signé.
 */
TEST_CASE("ping_roundtrip", "[comm_msg]")
{
    uint8_t buf[COMM_MSG_LORA_MAX];
    size_t out_len = 0;

    public_key_t key;
    fill_pubkey(&key, 0xBB);

    signature_t sig;
    fill_signature(&sig, 0x77);

    TEST_ASSERT_EQUAL(0, comm_msg_pack_ping(buf, sizeof(buf),
                                             &key, &sig, 42, &out_len));

    comm_msg_ping_t msg;
    TEST_ASSERT_EQUAL(0, comm_msg_unpack_ping(buf, out_len, &msg));

    TEST_ASSERT_EQUAL_MEMORY(key.bytes, msg.master_key.bytes, CRYPTO_PUBLIC_KEY_SIZE);
    TEST_ASSERT_EQUAL_MEMORY(sig.bytes, msg.signature.bytes, CRYPTO_SIGNATURE_SIZE);
    TEST_ASSERT_EQUAL(42, msg.ping_id);
}

/**
 * Test : pack PING avec buffer trop petit → erreur.
 */
TEST_CASE("ping_pack_buffer_too_small", "[comm_msg]")
{
    uint8_t buf[10];
    size_t out_len = 0;
    public_key_t key;
    fill_pubkey(&key, 0x11);
    signature_t sig;
    fill_signature(&sig, 0x22);

    TEST_ASSERT_EQUAL(-1, comm_msg_pack_ping(buf, sizeof(buf),
                                              &key, &sig, 1, &out_len));
}

/**
 * Test : unpack PING avec mauvais type → erreur.
 */
TEST_CASE("ping_unpack_wrong_type", "[comm_msg]")
{
    uint8_t buf[COMM_MSG_PING_SIZE];
    memset(buf, 0, sizeof(buf));
    buf[0] = COMM_MSG_LORA_PONG; /* Mauvais type */

    comm_msg_ping_t msg;
    TEST_ASSERT_EQUAL(-1, comm_msg_unpack_ping(buf, sizeof(buf), &msg));
}

/**
 * Test : pack PING avec params NULL → erreur.
 */
TEST_CASE("ping_pack_null_params", "[comm_msg]")
{
    uint8_t buf[COMM_MSG_LORA_MAX];
    size_t out_len = 0;
    public_key_t key;
    signature_t sig;

    TEST_ASSERT_EQUAL(-1, comm_msg_pack_ping(NULL, sizeof(buf), &key, &sig, 1, &out_len));
    TEST_ASSERT_EQUAL(-1, comm_msg_pack_ping(buf, sizeof(buf), NULL, &sig, 1, &out_len));
    TEST_ASSERT_EQUAL(-1, comm_msg_pack_ping(buf, sizeof(buf), &key, NULL, 1, &out_len));
    TEST_ASSERT_EQUAL(-1, comm_msg_pack_ping(buf, sizeof(buf), &key, &sig, 1, NULL));
}

/**
 * Test : comm_msg_get_type reconnaît LORA_PING.
 */
TEST_CASE("ping_get_type", "[comm_msg]")
{
    uint8_t buf[1] = { COMM_MSG_LORA_PING };
    comm_msg_type_t type;

    TEST_ASSERT_EQUAL(0, comm_msg_get_type(buf, 1, &type));
    TEST_ASSERT_EQUAL(COMM_MSG_LORA_PING, type);
}

/* ================================================================
 * Tests PONG
 * ================================================================ */

/**
 * Test : pack PONG — vérifie le format wire (version signée).
 * Format : [0x15][device_pubkey:32][sig:64][ping_id:2 BE][alias_len:1][alias:N]
 */
TEST_CASE("pong_pack_basic", "[comm_msg]")
{
    uint8_t buf[COMM_MSG_LORA_MAX];
    size_t out_len = 0;

    public_key_t key;
    fill_pubkey(&key, 0xCC);

    /* Signature factice (validité crypto testée séparément dans lora_sync) */
    signature_t sig;
    memset(sig.bytes, 0xAB, CRYPTO_SIGNATURE_SIZE);

    const char *alias = "Stand-A";
    uint8_t alias_len = (uint8_t)strlen(alias);

    int ret = comm_msg_pack_pong(buf, sizeof(buf),
                                 &key, &sig, 0x00FF,
                                 alias, alias_len, &out_len);
    TEST_ASSERT_EQUAL(0, ret);
    TEST_ASSERT_EQUAL(COMM_MSG_PONG_MIN_SIZE + alias_len, out_len);

    /* Type */
    TEST_ASSERT_EQUAL(COMM_MSG_LORA_PONG, buf[0]);

    /* pubkey (offset 1) */
    TEST_ASSERT_EQUAL_MEMORY(key.bytes, &buf[1], CRYPTO_PUBLIC_KEY_SIZE);

    /* signature (offset 33) */
    TEST_ASSERT_EQUAL_MEMORY(sig.bytes, &buf[33], CRYPTO_SIGNATURE_SIZE);

    /* ping_id BE (offset 97) */
    TEST_ASSERT_EQUAL(0x00, buf[97]);
    TEST_ASSERT_EQUAL(0xFF, buf[98]);

    /* alias_len + alias (offset 99+) */
    TEST_ASSERT_EQUAL(alias_len, buf[99]);
    TEST_ASSERT_EQUAL_MEMORY(alias, &buf[100], alias_len);
}

/**
 * Test : round-trip pack → unpack PONG.
 */
TEST_CASE("pong_roundtrip", "[comm_msg]")
{
    uint8_t buf[COMM_MSG_LORA_MAX];
    size_t out_len = 0;

    public_key_t key;
    fill_pubkey(&key, 0xDD);

    signature_t sig;
    memset(sig.bytes, 0x5C, CRYPTO_SIGNATURE_SIZE);

    const char *alias = "Caisse-3";
    uint8_t alias_len = (uint8_t)strlen(alias);

    TEST_ASSERT_EQUAL(0, comm_msg_pack_pong(buf, sizeof(buf),
                                             &key, &sig, 99,
                                             alias, alias_len, &out_len));

    comm_msg_pong_t msg;
    memset(&msg, 0, sizeof(msg));
    TEST_ASSERT_EQUAL(0, comm_msg_unpack_pong(buf, out_len, &msg));

    TEST_ASSERT_EQUAL(99, msg.ping_id);
    TEST_ASSERT_EQUAL_MEMORY(key.bytes, msg.device_key.bytes, CRYPTO_PUBLIC_KEY_SIZE);
    TEST_ASSERT_EQUAL_MEMORY(sig.bytes, msg.signature.bytes, CRYPTO_SIGNATURE_SIZE);
    TEST_ASSERT_EQUAL(alias_len, msg.alias_len);
    TEST_ASSERT_EQUAL_STRING(alias, msg.alias);
}

/**
 * Test : round-trip PONG avec alias max (32 chars).
 */
TEST_CASE("pong_roundtrip_max_alias", "[comm_msg]")
{
    uint8_t buf[COMM_MSG_LORA_MAX];
    size_t out_len = 0;

    public_key_t key;
    fill_pubkey(&key, 0xEE);

    signature_t sig;
    memset(sig.bytes, 0x42, CRYPTO_SIGNATURE_SIZE);

    char alias[COMM_MSG_ALIAS_MAX + 1];
    memset(alias, 'Z', COMM_MSG_ALIAS_MAX);
    alias[COMM_MSG_ALIAS_MAX] = '\0';

    TEST_ASSERT_EQUAL(0, comm_msg_pack_pong(buf, sizeof(buf),
                                             &key, &sig, 1,
                                             alias, COMM_MSG_ALIAS_MAX,
                                             &out_len));

    comm_msg_pong_t msg;
    TEST_ASSERT_EQUAL(0, comm_msg_unpack_pong(buf, out_len, &msg));

    TEST_ASSERT_EQUAL(COMM_MSG_ALIAS_MAX, msg.alias_len);
    TEST_ASSERT_EQUAL_MEMORY(alias, msg.alias, COMM_MSG_ALIAS_MAX);
}

/**
 * Test : round-trip PONG avec alias vide (len=0).
 */
TEST_CASE("pong_roundtrip_empty_alias", "[comm_msg]")
{
    uint8_t buf[COMM_MSG_LORA_MAX];
    size_t out_len = 0;

    public_key_t key;
    fill_pubkey(&key, 0xFF);

    signature_t sig;
    memset(sig.bytes, 0x77, CRYPTO_SIGNATURE_SIZE);

    TEST_ASSERT_EQUAL(0, comm_msg_pack_pong(buf, sizeof(buf),
                                             &key, &sig, 5,
                                             "", 0, &out_len));

    /* Taille min : 100 octets (pas d'alias) */
    TEST_ASSERT_EQUAL(COMM_MSG_PONG_MIN_SIZE, out_len);

    comm_msg_pong_t msg;
    TEST_ASSERT_EQUAL(0, comm_msg_unpack_pong(buf, out_len, &msg));
    TEST_ASSERT_EQUAL(0, msg.alias_len);
    TEST_ASSERT_EQUAL('\0', msg.alias[0]);
}

/**
 * Test : unpack PONG avec buffer trop court → erreur.
 */
TEST_CASE("pong_unpack_buffer_too_short", "[comm_msg]")
{
    uint8_t buf[20];
    buf[0] = COMM_MSG_LORA_PONG;

    comm_msg_pong_t msg;
    TEST_ASSERT_EQUAL(-1, comm_msg_unpack_pong(buf, sizeof(buf), &msg));
}

/**
 * Test : unpack PONG avec alias qui depasse le buffer → erreur.
 */
TEST_CASE("pong_unpack_alias_exceeds_buffer", "[comm_msg]")
{
    uint8_t buf[COMM_MSG_LORA_MAX];
    size_t out_len = 0;

    public_key_t key;
    fill_pubkey(&key, 0x11);

    signature_t sig;
    memset(sig.bytes, 0x33, CRYPTO_SIGNATURE_SIZE);

    /* Pack un PONG avec alias de 10 chars */
    comm_msg_pack_pong(buf, sizeof(buf),
                       &key, &sig, 1,
                       "0123456789", 10, &out_len);

    /* Tronquer le buffer pour que l'alias dépasse */
    comm_msg_pong_t msg;
    TEST_ASSERT_EQUAL(-1, comm_msg_unpack_pong(buf, COMM_MSG_PONG_MIN_SIZE + 5, &msg));
}

/**
 * Test : comm_msg_get_type reconnaît LORA_PONG.
 */
TEST_CASE("pong_get_type", "[comm_msg]")
{
    uint8_t buf[1] = { COMM_MSG_LORA_PONG };
    comm_msg_type_t type;

    TEST_ASSERT_EQUAL(0, comm_msg_get_type(buf, 1, &type));
    TEST_ASSERT_EQUAL(COMM_MSG_LORA_PONG, type);
}
