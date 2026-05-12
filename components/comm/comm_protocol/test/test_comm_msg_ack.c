/**
 * @file test_comm_msg_ack.c
 * @brief Tests unitaires du pack/unpack des messages TX_ACK signes.
 *
 * Verifie le round-trip, les constantes de taille et les cas d'erreur
 * (buffer trop court, mauvais type, parametres NULL).
 *
 * Format wire : [0x04][sender_pubkey:32][sig:64][nonce:4][tx_id:32]
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

/** Remplit un hash avec un pattern reconnaissable */
static void fill_hash(hash_t *h, uint8_t pattern)
{
    memset(h->bytes, pattern, CRYPTO_HASH_SIZE);
}

/* ================================================================
 * Tests de pack
 * ================================================================ */

/**
 * Test : pack ack signe normal -- verifie le format wire.
 * Format attendu : [0x04][sender_pubkey:32][sig:64][nonce:4][tx_id:32]
 */
TEST_CASE("ack_pack_basic", "[comm_msg]")
{
    uint8_t buf[COMM_MSG_ESPNOW_MAX];
    size_t out_len = 0;

    public_key_t key;
    fill_pubkey(&key, 0xAA);

    signature_t sig;
    fill_signature(&sig, 0xBB);

    hash_t tx_id;
    fill_hash(&tx_id, 0xCC);

    uint32_t nonce = 0xDEADBEEF;

    int ret = comm_msg_pack_ack(buf, sizeof(buf),
                                &key, &sig, nonce, &tx_id, &out_len);
    TEST_ASSERT_EQUAL(0, ret);

    /* Taille fixe : 133 octets */
    TEST_ASSERT_EQUAL(COMM_MSG_ACK_SIZE, out_len);

    /* Verifier le type */
    TEST_ASSERT_EQUAL(COMM_MSG_TX_ACK, buf[0]);

    /* Verifier la pubkey (octets 1..32) */
    TEST_ASSERT_EQUAL_MEMORY(key.bytes, &buf[1], CRYPTO_PUBLIC_KEY_SIZE);

    /* Verifier la signature (octets 33..96) */
    TEST_ASSERT_EQUAL_MEMORY(sig.bytes, &buf[33], CRYPTO_SIGNATURE_SIZE);

    /* Verifier le nonce (octets 97..100, big-endian) */
    TEST_ASSERT_EQUAL(0xDE, buf[97]);
    TEST_ASSERT_EQUAL(0xAD, buf[98]);
    TEST_ASSERT_EQUAL(0xBE, buf[99]);
    TEST_ASSERT_EQUAL(0xEF, buf[100]);

    /* Verifier le tx_id (octets 101..132) */
    TEST_ASSERT_EQUAL_MEMORY(tx_id.bytes, &buf[101], CRYPTO_HASH_SIZE);
}

/**
 * Test : pack avec buffer trop petit -- erreur.
 */
TEST_CASE("ack_pack_buffer_too_small", "[comm_msg]")
{
    uint8_t buf[50]; /* Trop petit pour un ACK signe (133 octets) */
    size_t out_len = 0;

    public_key_t key;
    fill_pubkey(&key, 0x11);

    signature_t sig;
    fill_signature(&sig, 0x22);

    hash_t tx_id;
    fill_hash(&tx_id, 0x33);

    int ret = comm_msg_pack_ack(buf, sizeof(buf),
                                &key, &sig, 0, &tx_id, &out_len);
    TEST_ASSERT_EQUAL(-1, ret);
}

/**
 * Test : pack avec parametres NULL -- erreur.
 */
TEST_CASE("ack_pack_null_params", "[comm_msg]")
{
    uint8_t buf[COMM_MSG_ESPNOW_MAX];
    size_t out_len = 0;
    public_key_t key;
    signature_t sig;
    hash_t tx_id;

    TEST_ASSERT_EQUAL(-1, comm_msg_pack_ack(NULL, sizeof(buf),
                                             &key, &sig, 0, &tx_id, &out_len));
    TEST_ASSERT_EQUAL(-1, comm_msg_pack_ack(buf, sizeof(buf),
                                             NULL, &sig, 0, &tx_id, &out_len));
    TEST_ASSERT_EQUAL(-1, comm_msg_pack_ack(buf, sizeof(buf),
                                             &key, NULL, 0, &tx_id, &out_len));
    TEST_ASSERT_EQUAL(-1, comm_msg_pack_ack(buf, sizeof(buf),
                                             &key, &sig, 0, NULL, &out_len));
    TEST_ASSERT_EQUAL(-1, comm_msg_pack_ack(buf, sizeof(buf),
                                             &key, &sig, 0, &tx_id, NULL));
}

/* ================================================================
 * Tests de unpack
 * ================================================================ */

/**
 * Test : round-trip pack -> unpack -- le contenu doit etre identique.
 */
TEST_CASE("ack_roundtrip", "[comm_msg]")
{
    uint8_t buf[COMM_MSG_ESPNOW_MAX];
    size_t out_len = 0;

    public_key_t key;
    fill_pubkey(&key, 0xCC);

    signature_t sig;
    fill_signature(&sig, 0xDD);

    hash_t tx_id;
    fill_hash(&tx_id, 0xEE);

    uint32_t nonce = 0xCAFEBABE;

    /* Pack */
    TEST_ASSERT_EQUAL(0, comm_msg_pack_ack(buf, sizeof(buf),
                                            &key, &sig, nonce, &tx_id, &out_len));

    /* Unpack */
    comm_msg_ack_t msg;
    memset(&msg, 0, sizeof(msg));
    TEST_ASSERT_EQUAL(0, comm_msg_unpack_ack(buf, out_len, &msg));

    /* Verifier que tout est preserve */
    TEST_ASSERT_EQUAL_MEMORY(key.bytes, msg.sender_key.bytes, CRYPTO_PUBLIC_KEY_SIZE);
    TEST_ASSERT_EQUAL_MEMORY(sig.bytes, msg.signature.bytes, CRYPTO_SIGNATURE_SIZE);
    TEST_ASSERT_EQUAL(nonce, msg.nonce);
    TEST_ASSERT_EQUAL_MEMORY(tx_id.bytes, msg.tx_id.bytes, CRYPTO_HASH_SIZE);
}

/**
 * Test : unpack avec buffer trop court -- erreur.
 */
TEST_CASE("ack_unpack_buffer_too_short", "[comm_msg]")
{
    /* Buffer de 50 octets -- en dessous du minimum de 133 */
    uint8_t buf[50];
    buf[0] = COMM_MSG_TX_ACK;

    comm_msg_ack_t msg;
    TEST_ASSERT_EQUAL(-1, comm_msg_unpack_ack(buf, sizeof(buf), &msg));
}

/**
 * Test : unpack avec mauvais type -- erreur.
 */
TEST_CASE("ack_unpack_wrong_type", "[comm_msg]")
{
    uint8_t buf[COMM_MSG_ESPNOW_MAX];
    memset(buf, 0, sizeof(buf));
    buf[0] = COMM_MSG_DISCOVER; /* Mauvais type */

    comm_msg_ack_t msg;
    TEST_ASSERT_EQUAL(-1, comm_msg_unpack_ack(buf, sizeof(buf), &msg));
}

/**
 * Test : comm_msg_get_type reconnait TX_ACK.
 */
TEST_CASE("ack_get_type", "[comm_msg]")
{
    uint8_t buf[1] = { COMM_MSG_TX_ACK };
    comm_msg_type_t type;

    TEST_ASSERT_EQUAL(0, comm_msg_get_type(buf, 1, &type));
    TEST_ASSERT_EQUAL(COMM_MSG_TX_ACK, type);
}

/**
 * Test : verification de la constante COMM_MSG_ACK_SIZE.
 * 1 (type) + 32 (pubkey) + 64 (sig) + 4 (nonce) + 32 (tx_id) = 133
 */
TEST_CASE("ack_size_constant", "[comm_msg]")
{
    TEST_ASSERT_EQUAL(133, COMM_MSG_ACK_SIZE);
}
