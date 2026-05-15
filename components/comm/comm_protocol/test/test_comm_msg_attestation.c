/**
 * @file test_comm_msg_attestation.c
 * @brief [I2-fix] Tests unitaires du pack/unpack des messages ATTESTATION.
 *
 * Format wire : [0x18][attester_pubkey:32][sig:64][tx_id:32] = 129 octets fixes
 *
 * L'attestation LoRa est diffusee par le destinataire d'une TX pour
 * prouver cryptographiquement qu'il a bien recu et accepte la TX.
 * Cela permet au reste du reseau (hors portee ESP-NOW) de promouvoir
 * la TX de LOCKED a CONFIRMED.
 */

#include "unity.h"
#include "comm/comm_msg.h"
#include <string.h>


/* Helpers de remplissage de buffers avec patterns reconnaissables */
static void fill_pubkey(public_key_t *key, uint8_t pattern)
{
    memset(key->bytes, pattern, CRYPTO_PUBLIC_KEY_SIZE);
}
static void fill_signature(signature_t *sig, uint8_t pattern)
{
    memset(sig->bytes, pattern, CRYPTO_SIGNATURE_SIZE);
}
static void fill_hash(hash_t *h, uint8_t pattern)
{
    memset(h->bytes, pattern, CRYPTO_HASH_SIZE);
}

/* ================================================================
 * Tests ATTESTATION
 * ================================================================ */

/**
 * Test : pack ATTESTATION — verifie le format wire.
 */
TEST_CASE("attestation_pack_basic", "[comm_msg]")
{
    uint8_t buf[COMM_MSG_LORA_MAX];
    size_t out_len = 0;

    public_key_t key;
    fill_pubkey(&key, 0xA1);
    signature_t sig;
    fill_signature(&sig, 0xB2);
    hash_t tx_id;
    fill_hash(&tx_id, 0xC3);

    int ret = comm_msg_pack_attestation(buf, sizeof(buf),
                                        &key, &sig, &tx_id, &out_len);
    TEST_ASSERT_EQUAL(0, ret);
    TEST_ASSERT_EQUAL(COMM_MSG_ATTESTATION_SIZE, out_len);
    TEST_ASSERT_EQUAL(129, out_len);  /* Constante documentee */

    /* Type */
    TEST_ASSERT_EQUAL(COMM_MSG_LORA_ATTESTATION, buf[0]);
    /* pubkey a offset 1 */
    TEST_ASSERT_EQUAL_MEMORY(key.bytes, &buf[1], CRYPTO_PUBLIC_KEY_SIZE);
    /* signature a offset 33 */
    TEST_ASSERT_EQUAL_MEMORY(sig.bytes, &buf[33], CRYPTO_SIGNATURE_SIZE);
    /* tx_id a offset 97 */
    TEST_ASSERT_EQUAL_MEMORY(tx_id.bytes, &buf[97], CRYPTO_HASH_SIZE);
}

/**
 * Test : round-trip pack → unpack.
 */
TEST_CASE("attestation_roundtrip", "[comm_msg]")
{
    uint8_t buf[COMM_MSG_LORA_MAX];
    size_t out_len = 0;

    public_key_t key;
    fill_pubkey(&key, 0x55);
    signature_t sig;
    fill_signature(&sig, 0xAA);
    hash_t tx_id;
    fill_hash(&tx_id, 0xFF);

    TEST_ASSERT_EQUAL(0, comm_msg_pack_attestation(buf, sizeof(buf),
                                                    &key, &sig, &tx_id,
                                                    &out_len));

    comm_msg_attestation_t msg;
    memset(&msg, 0, sizeof(msg));
    TEST_ASSERT_EQUAL(0, comm_msg_unpack_attestation(buf, out_len, &msg));

    TEST_ASSERT_EQUAL_MEMORY(key.bytes, msg.attester_key.bytes,
                             CRYPTO_PUBLIC_KEY_SIZE);
    TEST_ASSERT_EQUAL_MEMORY(sig.bytes, msg.signature.bytes,
                             CRYPTO_SIGNATURE_SIZE);
    TEST_ASSERT_EQUAL_MEMORY(tx_id.bytes, msg.tx_id.bytes,
                             CRYPTO_HASH_SIZE);
}

/**
 * Test : unpack avec buffer trop court → erreur.
 */
TEST_CASE("attestation_unpack_buffer_too_short", "[comm_msg]")
{
    uint8_t buf[COMM_MSG_ATTESTATION_SIZE - 1];
    memset(buf, 0, sizeof(buf));
    buf[0] = COMM_MSG_LORA_ATTESTATION;

    comm_msg_attestation_t msg;
    TEST_ASSERT_EQUAL(-1, comm_msg_unpack_attestation(buf, sizeof(buf), &msg));
}

/**
 * Test : unpack avec mauvais type → erreur.
 */
TEST_CASE("attestation_unpack_wrong_type", "[comm_msg]")
{
    uint8_t buf[COMM_MSG_ATTESTATION_SIZE];
    memset(buf, 0xAB, sizeof(buf));
    buf[0] = 0x10;  /* COMM_MSG_LORA_TX, pas ATTESTATION */

    comm_msg_attestation_t msg;
    TEST_ASSERT_EQUAL(-1, comm_msg_unpack_attestation(buf, sizeof(buf), &msg));
}

/**
 * Test : pack avec buffer trop petit → erreur.
 */
TEST_CASE("attestation_pack_buffer_too_small", "[comm_msg]")
{
    uint8_t buf[COMM_MSG_ATTESTATION_SIZE - 1];
    size_t out_len = 0;

    public_key_t key;
    signature_t sig;
    hash_t tx_id;
    fill_pubkey(&key, 0x01);
    fill_signature(&sig, 0x02);
    fill_hash(&tx_id, 0x03);

    int ret = comm_msg_pack_attestation(buf, sizeof(buf),
                                        &key, &sig, &tx_id, &out_len);
    TEST_ASSERT_EQUAL(-1, ret);
}

/**
 * Test : NULL args → erreur.
 */
TEST_CASE("attestation_pack_null_args", "[comm_msg]")
{
    uint8_t buf[COMM_MSG_LORA_MAX];
    size_t out_len = 0;
    public_key_t key;
    signature_t sig;
    hash_t tx_id;
    fill_pubkey(&key, 0x01);
    fill_signature(&sig, 0x02);
    fill_hash(&tx_id, 0x03);

    TEST_ASSERT_EQUAL(-1, comm_msg_pack_attestation(NULL, sizeof(buf),
                                                     &key, &sig, &tx_id, &out_len));
    TEST_ASSERT_EQUAL(-1, comm_msg_pack_attestation(buf, sizeof(buf),
                                                     NULL, &sig, &tx_id, &out_len));
    TEST_ASSERT_EQUAL(-1, comm_msg_pack_attestation(buf, sizeof(buf),
                                                     &key, NULL, &tx_id, &out_len));
    TEST_ASSERT_EQUAL(-1, comm_msg_pack_attestation(buf, sizeof(buf),
                                                     &key, &sig, NULL, &out_len));
    TEST_ASSERT_EQUAL(-1, comm_msg_pack_attestation(buf, sizeof(buf),
                                                     &key, &sig, &tx_id, NULL));
}

/**
 * Test : comm_msg_get_type reconnait LORA_ATTESTATION.
 */
TEST_CASE("attestation_get_type", "[comm_msg]")
{
    /* Lot E.1bis : fix dans comm_msg.c (case oublie dans le switch
     * de comm_msg_get_type). */
    uint8_t buf[1] = { COMM_MSG_LORA_ATTESTATION };
    comm_msg_type_t type;

    TEST_ASSERT_EQUAL(0, comm_msg_get_type(buf, 1, &type));
    TEST_ASSERT_EQUAL(COMM_MSG_LORA_ATTESTATION, type);
}

/* ================================================================
 * Tests verify_attestation [F-CP-003, audit 2026-05-15]
 *
 * Symetrique aux comm_msg_verify_{broadcast,set_alias,set_beneficiary}.
 * Canonical signed buffer = [tx_id:32], signe par attester_key.
 * ================================================================ */

#include "crypto/crypto_sign.h"
#include "crypto/crypto_init.h"
#include "crypto/crypto_keys.h"

/**
 * Helper : signe un tx_id avec la cle privee fournie et remplit msg.
 */
static void build_signed_attestation(const keypair_t *kp,
                                      const hash_t *tx_id,
                                      comm_msg_attestation_t *out_msg)
{
    signature_t sig;
    TEST_ASSERT_EQUAL(ESP_OK,
        crypto_sign(tx_id->bytes, CRYPTO_HASH_SIZE, kp, &sig));

    memcpy(&out_msg->attester_key, &kp->public_key, sizeof(public_key_t));
    memcpy(&out_msg->signature, &sig, sizeof(signature_t));
    memcpy(&out_msg->tx_id, tx_id, sizeof(hash_t));
}

/**
 * Cas nominal : une attestation correctement signee doit etre acceptee.
 */
TEST_CASE("attestation_verify_signature_valide", "[comm_msg]")
{
    TEST_ASSERT_EQUAL(ESP_OK, crypto_init());

    keypair_t kp;
    TEST_ASSERT_EQUAL(ESP_OK, crypto_generate_keypair(&kp));

    hash_t tx_id;
    fill_hash(&tx_id, 0x77);

    comm_msg_attestation_t msg;
    build_signed_attestation(&kp, &tx_id, &msg);

    TEST_ASSERT_EQUAL(0, comm_msg_verify_attestation(&msg));
}

/**
 * Signature garbage : doit etre rejetee.
 */
TEST_CASE("attestation_verify_signature_invalide", "[comm_msg]")
{
    TEST_ASSERT_EQUAL(ESP_OK, crypto_init());

    keypair_t kp;
    crypto_generate_keypair(&kp);

    comm_msg_attestation_t msg;
    memcpy(&msg.attester_key, &kp.public_key, sizeof(public_key_t));
    fill_signature(&msg.signature, 0xFF); /* garbage */
    fill_hash(&msg.tx_id, 0x33);

    TEST_ASSERT_EQUAL(-1, comm_msg_verify_attestation(&msg));
}

/**
 * Alteration du tx_id apres signature : rejet attendu.
 * Verifie l'integrite cryptographique du champ signe.
 */
TEST_CASE("attestation_verify_alteration_tx_id_detectee", "[comm_msg]")
{
    TEST_ASSERT_EQUAL(ESP_OK, crypto_init());

    keypair_t kp;
    crypto_generate_keypair(&kp);

    hash_t tx_id;
    fill_hash(&tx_id, 0xA5);

    comm_msg_attestation_t msg;
    build_signed_attestation(&kp, &tx_id, &msg);

    /* Sanity */
    TEST_ASSERT_EQUAL(0, comm_msg_verify_attestation(&msg));

    /* Alterer un octet du tx_id apres signature */
    msg.tx_id.bytes[0] ^= 0xFF;
    TEST_ASSERT_EQUAL(-1, comm_msg_verify_attestation(&msg));
}

/**
 * Signature valide sous une AUTRE cle : rejet attendu.
 * (Empeche un attaquant de substituer attester_key pour faire passer
 * une signature pour son compte.)
 */
TEST_CASE("attestation_verify_mauvais_signataire", "[comm_msg]")
{
    TEST_ASSERT_EQUAL(ESP_OK, crypto_init());

    keypair_t kp_signer;
    keypair_t kp_impostor;
    crypto_generate_keypair(&kp_signer);
    crypto_generate_keypair(&kp_impostor);

    hash_t tx_id;
    fill_hash(&tx_id, 0x42);

    comm_msg_attestation_t msg;
    build_signed_attestation(&kp_signer, &tx_id, &msg);

    /* Sanity */
    TEST_ASSERT_EQUAL(0, comm_msg_verify_attestation(&msg));

    /* Remplacer attester_key par celle de l'imposteur */
    memcpy(&msg.attester_key, &kp_impostor.public_key, sizeof(public_key_t));
    TEST_ASSERT_EQUAL(-1, comm_msg_verify_attestation(&msg));
}

/**
 * Argument NULL : rejet attendu.
 */
TEST_CASE("attestation_verify_null", "[comm_msg]")
{
    TEST_ASSERT_EQUAL(-1, comm_msg_verify_attestation(NULL));
}
