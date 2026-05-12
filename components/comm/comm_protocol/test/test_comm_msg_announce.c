/**
 * @file test_comm_msg_announce.c
 * @brief Tests unitaires du pack/unpack des messages ANNOUNCE signés.
 *
 * Vérifie le round-trip, les constantes de taille, les cas limites
 * (alias max, alias vide) et les cas d'erreur (buffer trop court, NULL).
 *
 * Format wire : [0x02][pubkey:32][sig:64][nonce:4][alias_len:1][alias:N]
 */

#include "unity.h"
#include "comm/comm_msg.h"
#include <string.h>

void setUp(void) {}
void tearDown(void) {}

/* ================================================================
 * Helpers -- donnees de test
 * ================================================================ */

/** Remplit une pubkey avec un pattern reconnaissable */
static void fill_pubkey(public_key_t *key, uint8_t pattern)
{
    memset(key->bytes, pattern, CRYPTO_PUBLIC_KEY_SIZE);
}

/** Remplit une signature avec un pattern reconnaissable */
static void fill_signature(signature_t *sig, uint8_t pattern)
{
    memset(sig->bytes, pattern, CRYPTO_SIGNATURE_SIZE);
}

/* ================================================================
 * Tests de pack
 * ================================================================ */

/**
 * Test : pack announce signe normal -- verifie le format wire.
 * Format attendu : [0x02][pubkey:32][sig:64][nonce:4][alias_len:1][alias:N]
 */
TEST_CASE("announce_pack_basic", "[comm_msg]")
{
    uint8_t buf[COMM_MSG_ESPNOW_MAX];
    size_t out_len = 0;

    public_key_t key;
    fill_pubkey(&key, 0xAA);

    signature_t sig;
    fill_signature(&sig, 0xBB);

    uint32_t nonce = 0xDEADBEEF;
    const char *alias = "MonDevice";
    uint8_t alias_len = (uint8_t)strlen(alias);

    int ret = comm_msg_pack_announce(buf, sizeof(buf),
                                     &key, &sig, nonce,
                                     alias, alias_len, &out_len);
    TEST_ASSERT_EQUAL(0, ret);

    /* Taille : 1 + 32 + 64 + 4 + 1 + 9 = 111 */
    TEST_ASSERT_EQUAL(COMM_MSG_ANNOUNCE_MIN_SIZE + alias_len, out_len);

    /* Verifier le type */
    TEST_ASSERT_EQUAL(COMM_MSG_ANNOUNCE, buf[0]);

    /* Verifier la pubkey (octets 1..32) */
    TEST_ASSERT_EQUAL_MEMORY(key.bytes, &buf[1], CRYPTO_PUBLIC_KEY_SIZE);

    /* Verifier la signature (octets 33..96) */
    TEST_ASSERT_EQUAL_MEMORY(sig.bytes, &buf[33], CRYPTO_SIGNATURE_SIZE);

    /* Verifier le nonce (octets 97..100, big-endian) */
    TEST_ASSERT_EQUAL(0xDE, buf[97]);
    TEST_ASSERT_EQUAL(0xAD, buf[98]);
    TEST_ASSERT_EQUAL(0xBE, buf[99]);
    TEST_ASSERT_EQUAL(0xEF, buf[100]);

    /* Verifier alias_len (octet 101) */
    TEST_ASSERT_EQUAL(alias_len, buf[101]);

    /* Verifier l'alias (octets 102..110) */
    TEST_ASSERT_EQUAL_MEMORY(alias, &buf[102], alias_len);
}

/**
 * Test : pack avec alias de taille max (32 chars).
 */
TEST_CASE("announce_pack_max_alias", "[comm_msg]")
{
    uint8_t buf[COMM_MSG_ESPNOW_MAX];
    size_t out_len = 0;

    public_key_t key;
    fill_pubkey(&key, 0x11);

    signature_t sig;
    fill_signature(&sig, 0x22);

    /* Remplir un alias de 32 chars */
    char alias[COMM_MSG_ALIAS_MAX + 1];
    memset(alias, 'A', COMM_MSG_ALIAS_MAX);
    alias[COMM_MSG_ALIAS_MAX] = '\0';

    int ret = comm_msg_pack_announce(buf, sizeof(buf),
                                     &key, &sig, 0x12345678,
                                     alias, COMM_MSG_ALIAS_MAX, &out_len);
    TEST_ASSERT_EQUAL(0, ret);

    /* Taille max : 102 + 32 = 134 (sous la limite ESP-NOW de 250) */
    TEST_ASSERT_EQUAL(COMM_MSG_ANNOUNCE_MIN_SIZE + COMM_MSG_ALIAS_MAX, out_len);
}

/**
 * Test : pack avec buffer trop petit -- erreur.
 */
TEST_CASE("announce_pack_buffer_too_small", "[comm_msg]")
{
    uint8_t buf[50]; /* Trop petit pour un ANNOUNCE signe (min 102) */
    size_t out_len = 0;

    public_key_t key;
    fill_pubkey(&key, 0x11);

    signature_t sig;
    fill_signature(&sig, 0x22);

    int ret = comm_msg_pack_announce(buf, sizeof(buf),
                                     &key, &sig, 0, "Hi", 2, &out_len);
    TEST_ASSERT_EQUAL(-1, ret);
}

/**
 * Test : pack avec parametres NULL -- erreur.
 */
TEST_CASE("announce_pack_null_params", "[comm_msg]")
{
    uint8_t buf[COMM_MSG_ESPNOW_MAX];
    size_t out_len = 0;
    public_key_t key;
    signature_t sig;

    TEST_ASSERT_EQUAL(-1, comm_msg_pack_announce(NULL, sizeof(buf),
                                                  &key, &sig, 0, "a", 1, &out_len));
    TEST_ASSERT_EQUAL(-1, comm_msg_pack_announce(buf, sizeof(buf),
                                                  NULL, &sig, 0, "a", 1, &out_len));
    TEST_ASSERT_EQUAL(-1, comm_msg_pack_announce(buf, sizeof(buf),
                                                  &key, NULL, 0, "a", 1, &out_len));
    TEST_ASSERT_EQUAL(-1, comm_msg_pack_announce(buf, sizeof(buf),
                                                  &key, &sig, 0, NULL, 1, &out_len));
    TEST_ASSERT_EQUAL(-1, comm_msg_pack_announce(buf, sizeof(buf),
                                                  &key, &sig, 0, "a", 1, NULL));
}

/* ================================================================
 * Tests de unpack
 * ================================================================ */

/**
 * Test : round-trip pack -> unpack -- le contenu doit etre identique.
 */
TEST_CASE("announce_roundtrip", "[comm_msg]")
{
    uint8_t buf[COMM_MSG_ESPNOW_MAX];
    size_t out_len = 0;

    public_key_t key;
    fill_pubkey(&key, 0xCC);

    signature_t sig;
    fill_signature(&sig, 0xDD);

    uint32_t nonce = 0xCAFEBABE;
    const char *alias = "TestPeer";
    uint8_t alias_len = (uint8_t)strlen(alias);

    /* Pack */
    TEST_ASSERT_EQUAL(0, comm_msg_pack_announce(buf, sizeof(buf),
                                                 &key, &sig, nonce,
                                                 alias, alias_len, &out_len));

    /* Unpack */
    comm_msg_announce_t msg;
    memset(&msg, 0, sizeof(msg));
    TEST_ASSERT_EQUAL(0, comm_msg_unpack_announce(buf, out_len, &msg));

    /* Verifier que tout est preserve */
    TEST_ASSERT_EQUAL_MEMORY(key.bytes, msg.device_key.bytes, CRYPTO_PUBLIC_KEY_SIZE);
    TEST_ASSERT_EQUAL_MEMORY(sig.bytes, msg.signature.bytes, CRYPTO_SIGNATURE_SIZE);
    TEST_ASSERT_EQUAL(nonce, msg.nonce);
    TEST_ASSERT_EQUAL(alias_len, msg.alias_len);
    TEST_ASSERT_EQUAL_STRING(alias, msg.alias);
}

/**
 * Test : round-trip avec alias max (32 chars).
 */
TEST_CASE("announce_roundtrip_max", "[comm_msg]")
{
    uint8_t buf[COMM_MSG_ESPNOW_MAX];
    size_t out_len = 0;

    public_key_t key;
    fill_pubkey(&key, 0x55);

    signature_t sig;
    fill_signature(&sig, 0x66);

    char alias[COMM_MSG_ALIAS_MAX + 1];
    memset(alias, 'Z', COMM_MSG_ALIAS_MAX);
    alias[COMM_MSG_ALIAS_MAX] = '\0';

    TEST_ASSERT_EQUAL(0, comm_msg_pack_announce(buf, sizeof(buf),
                                                 &key, &sig, 0xFFFFFFFF,
                                                 alias, COMM_MSG_ALIAS_MAX,
                                                 &out_len));

    comm_msg_announce_t msg;
    TEST_ASSERT_EQUAL(0, comm_msg_unpack_announce(buf, out_len, &msg));

    TEST_ASSERT_EQUAL(COMM_MSG_ALIAS_MAX, msg.alias_len);
    TEST_ASSERT_EQUAL_MEMORY(alias, msg.alias, COMM_MSG_ALIAS_MAX);
    TEST_ASSERT_EQUAL('\0', msg.alias[COMM_MSG_ALIAS_MAX]);
    TEST_ASSERT_EQUAL(0xFFFFFFFF, msg.nonce);
}

/**
 * Test : unpack avec buffer trop court -- erreur.
 */
TEST_CASE("announce_unpack_buffer_too_short", "[comm_msg]")
{
    /* Buffer de 50 octets -- en dessous du minimum de 102 */
    uint8_t buf[50];
    buf[0] = COMM_MSG_ANNOUNCE;

    comm_msg_announce_t msg;
    TEST_ASSERT_EQUAL(-1, comm_msg_unpack_announce(buf, sizeof(buf), &msg));
}

/**
 * Test : unpack avec mauvais type -- erreur.
 */
TEST_CASE("announce_unpack_wrong_type", "[comm_msg]")
{
    uint8_t buf[COMM_MSG_ESPNOW_MAX];
    memset(buf, 0, sizeof(buf));
    buf[0] = COMM_MSG_DISCOVER; /* Mauvais type */

    comm_msg_announce_t msg;
    TEST_ASSERT_EQUAL(-1, comm_msg_unpack_announce(buf, sizeof(buf), &msg));
}

/**
 * Test : unpack avec alias_len qui depasse la fin du buffer -- erreur.
 */
TEST_CASE("announce_unpack_alias_exceeds_buffer", "[comm_msg]")
{
    uint8_t buf[COMM_MSG_ESPNOW_MAX];
    size_t out_len = 0;

    public_key_t key;
    fill_pubkey(&key, 0x11);

    signature_t sig;
    fill_signature(&sig, 0x22);

    /* Pack un message avec alias de 10 chars */
    comm_msg_pack_announce(buf, sizeof(buf),
                           &key, &sig, 0, "0123456789", 10, &out_len);

    /* Tronquer le buffer pour que l'alias depasse */
    comm_msg_announce_t msg;
    TEST_ASSERT_EQUAL(-1, comm_msg_unpack_announce(buf, COMM_MSG_ANNOUNCE_MIN_SIZE + 5, &msg));
}

/**
 * Test : comm_msg_get_type reconnait ANNOUNCE.
 */
TEST_CASE("announce_get_type", "[comm_msg]")
{
    uint8_t buf[1] = { COMM_MSG_ANNOUNCE };
    comm_msg_type_t type;

    TEST_ASSERT_EQUAL(0, comm_msg_get_type(buf, 1, &type));
    TEST_ASSERT_EQUAL(COMM_MSG_ANNOUNCE, type);
}
