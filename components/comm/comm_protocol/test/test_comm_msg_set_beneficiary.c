/**
 * @file test_comm_msg_set_beneficiary.c
 * @brief Tests unitaires du pack/unpack des messages LORA_SET_BENEFICIARY.
 *
 * Verifie le round-trip, la desactivation (cle all-zeros), les cas limites
 * et les cas d'erreur (buffer trop court, type incorrect, NULL).
 */

#include "unity.h"
#include "comm/comm_msg.h"
#include "crypto/crypto_init.h"
#include "crypto/crypto_keys.h"
#include "crypto/crypto_sign.h"
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
 * Tests SET_BENEFICIARY — Pack
 * ================================================================ */

/**
 * Test : pack SET_BENEFICIARY — verifie le format wire (163 octets fixes).
 * Format : [0x17][master_key:32][sig:64][target_key:32][beneficiary_key:32][interval:2 BE]
 */
TEST_CASE("set_beneficiary_pack_basic", "[comm_msg]")
{
    uint8_t buf[COMM_MSG_LORA_MAX];
    size_t out_len = 0;

    public_key_t master_key, target_key, beneficiary_key;
    fill_pubkey(&master_key, 0xAA);
    fill_pubkey(&target_key, 0xBB);
    fill_pubkey(&beneficiary_key, 0xCC);

    signature_t sig;
    fill_signature(&sig, 0xDD);

    uint16_t interval = 60; /* 1 heure */

    int ret = comm_msg_pack_set_beneficiary(buf, sizeof(buf),
                                             &master_key, &sig, &target_key,
                                             &beneficiary_key, interval,
                                             &out_len);
    TEST_ASSERT_EQUAL(0, ret);
    TEST_ASSERT_EQUAL(COMM_MSG_SET_BENEFICIARY_SIZE, out_len);

    /* Verifier le type */
    TEST_ASSERT_EQUAL(COMM_MSG_LORA_SET_BENEFICIARY, buf[0]);

    /* Verifier la master_key (offset 1) */
    TEST_ASSERT_EQUAL_MEMORY(master_key.bytes, &buf[1], CRYPTO_PUBLIC_KEY_SIZE);

    /* Verifier la signature (offset 33) */
    TEST_ASSERT_EQUAL_MEMORY(sig.bytes, &buf[33], CRYPTO_SIGNATURE_SIZE);

    /* Verifier la target_key (offset 97) */
    TEST_ASSERT_EQUAL_MEMORY(target_key.bytes, &buf[97], CRYPTO_PUBLIC_KEY_SIZE);

    /* Verifier la beneficiary_key (offset 129) */
    TEST_ASSERT_EQUAL_MEMORY(beneficiary_key.bytes, &buf[129], CRYPTO_PUBLIC_KEY_SIZE);

    /* Verifier l'intervalle (offset 161, big-endian) */
    TEST_ASSERT_EQUAL(0, buf[161]);  /* 60 >> 8 = 0 */
    TEST_ASSERT_EQUAL(60, buf[162]); /* 60 & 0xFF = 60 */
}

/* ================================================================
 * Tests SET_BENEFICIARY — Round-trip
 * ================================================================ */

/**
 * Test : round-trip pack -> unpack SET_BENEFICIARY.
 */
TEST_CASE("set_beneficiary_roundtrip", "[comm_msg]")
{
    uint8_t buf[COMM_MSG_LORA_MAX];
    size_t out_len = 0;

    public_key_t master_key, target_key, beneficiary_key;
    fill_pubkey(&master_key, 0x11);
    fill_pubkey(&target_key, 0x22);
    fill_pubkey(&beneficiary_key, 0x33);

    signature_t sig;
    fill_signature(&sig, 0x44);

    uint16_t interval = 120; /* 2 heures */

    TEST_ASSERT_EQUAL(0, comm_msg_pack_set_beneficiary(buf, sizeof(buf),
                                                        &master_key, &sig,
                                                        &target_key,
                                                        &beneficiary_key,
                                                        interval, &out_len));

    comm_msg_set_beneficiary_t msg;
    memset(&msg, 0, sizeof(msg));
    TEST_ASSERT_EQUAL(0, comm_msg_unpack_set_beneficiary(buf, out_len, &msg));

    TEST_ASSERT_EQUAL_MEMORY(master_key.bytes, msg.master_key.bytes, CRYPTO_PUBLIC_KEY_SIZE);
    TEST_ASSERT_EQUAL_MEMORY(sig.bytes, msg.signature.bytes, CRYPTO_SIGNATURE_SIZE);
    TEST_ASSERT_EQUAL_MEMORY(target_key.bytes, msg.target_key.bytes, CRYPTO_PUBLIC_KEY_SIZE);
    TEST_ASSERT_EQUAL_MEMORY(beneficiary_key.bytes, msg.beneficiary_key.bytes, CRYPTO_PUBLIC_KEY_SIZE);
    TEST_ASSERT_EQUAL(interval, msg.forward_interval_min);
}

/**
 * Test : round-trip avec desactivation (beneficiary_key all-zeros, interval 0).
 */
TEST_CASE("set_beneficiary_roundtrip_deactivation", "[comm_msg]")
{
    uint8_t buf[COMM_MSG_LORA_MAX];
    size_t out_len = 0;

    public_key_t master_key, target_key, zero_key;
    fill_pubkey(&master_key, 0x55);
    fill_pubkey(&target_key, 0x66);
    memset(&zero_key, 0, sizeof(public_key_t));

    signature_t sig;
    fill_signature(&sig, 0x77);

    TEST_ASSERT_EQUAL(0, comm_msg_pack_set_beneficiary(buf, sizeof(buf),
                                                        &master_key, &sig,
                                                        &target_key,
                                                        &zero_key, 0,
                                                        &out_len));

    comm_msg_set_beneficiary_t msg;
    TEST_ASSERT_EQUAL(0, comm_msg_unpack_set_beneficiary(buf, out_len, &msg));

    /* Verifier que la cle est bien all-zeros */
    public_key_t expected_zero;
    memset(&expected_zero, 0, sizeof(public_key_t));
    TEST_ASSERT_EQUAL_MEMORY(expected_zero.bytes, msg.beneficiary_key.bytes, CRYPTO_PUBLIC_KEY_SIZE);
    TEST_ASSERT_EQUAL(0, msg.forward_interval_min);
}

/**
 * Test : round-trip avec intervalle max (65535 = ~45 jours).
 */
TEST_CASE("set_beneficiary_roundtrip_max_interval", "[comm_msg]")
{
    uint8_t buf[COMM_MSG_LORA_MAX];
    size_t out_len = 0;

    public_key_t master_key, target_key, beneficiary_key;
    fill_pubkey(&master_key, 0x88);
    fill_pubkey(&target_key, 0x99);
    fill_pubkey(&beneficiary_key, 0xAA);

    signature_t sig;
    fill_signature(&sig, 0xBB);

    uint16_t interval = UINT16_MAX;

    TEST_ASSERT_EQUAL(0, comm_msg_pack_set_beneficiary(buf, sizeof(buf),
                                                        &master_key, &sig,
                                                        &target_key,
                                                        &beneficiary_key,
                                                        interval, &out_len));

    comm_msg_set_beneficiary_t msg;
    TEST_ASSERT_EQUAL(0, comm_msg_unpack_set_beneficiary(buf, out_len, &msg));
    TEST_ASSERT_EQUAL(UINT16_MAX, msg.forward_interval_min);
}

/* ================================================================
 * Tests SET_BENEFICIARY — Erreurs
 * ================================================================ */

/**
 * Test : pack avec buffer trop petit -> erreur.
 */
TEST_CASE("set_beneficiary_pack_buffer_too_small", "[comm_msg]")
{
    uint8_t buf[100]; /* Trop petit pour 163 octets */
    size_t out_len = 0;

    public_key_t master_key, target_key, beneficiary_key;
    fill_pubkey(&master_key, 0x11);
    fill_pubkey(&target_key, 0x22);
    fill_pubkey(&beneficiary_key, 0x33);

    signature_t sig;
    fill_signature(&sig, 0x44);

    TEST_ASSERT_EQUAL(-1, comm_msg_pack_set_beneficiary(buf, sizeof(buf),
                                                         &master_key, &sig,
                                                         &target_key,
                                                         &beneficiary_key,
                                                         60, &out_len));
}

/**
 * Test : pack avec params NULL -> erreur.
 */
TEST_CASE("set_beneficiary_pack_null_params", "[comm_msg]")
{
    uint8_t buf[COMM_MSG_LORA_MAX];
    size_t out_len = 0;
    public_key_t key = {0};
    signature_t sig = {0};

    TEST_ASSERT_EQUAL(-1, comm_msg_pack_set_beneficiary(NULL, sizeof(buf), &key, &sig, &key, &key, 60, &out_len));
    TEST_ASSERT_EQUAL(-1, comm_msg_pack_set_beneficiary(buf, sizeof(buf), NULL, &sig, &key, &key, 60, &out_len));
    TEST_ASSERT_EQUAL(-1, comm_msg_pack_set_beneficiary(buf, sizeof(buf), &key, NULL, &key, &key, 60, &out_len));
    TEST_ASSERT_EQUAL(-1, comm_msg_pack_set_beneficiary(buf, sizeof(buf), &key, &sig, NULL, &key, 60, &out_len));
    TEST_ASSERT_EQUAL(-1, comm_msg_pack_set_beneficiary(buf, sizeof(buf), &key, &sig, &key, NULL, 60, &out_len));
    TEST_ASSERT_EQUAL(-1, comm_msg_pack_set_beneficiary(buf, sizeof(buf), &key, &sig, &key, &key, 60, NULL));
}

/**
 * Test : unpack avec mauvais type -> erreur.
 */
TEST_CASE("set_beneficiary_unpack_wrong_type", "[comm_msg]")
{
    uint8_t buf[COMM_MSG_SET_BENEFICIARY_SIZE];
    memset(buf, 0, sizeof(buf));
    buf[0] = COMM_MSG_LORA_PING; /* Mauvais type */

    comm_msg_set_beneficiary_t msg;
    TEST_ASSERT_EQUAL(-1, comm_msg_unpack_set_beneficiary(buf, sizeof(buf), &msg));
}

/**
 * Test : unpack avec buffer trop court -> erreur.
 */
TEST_CASE("set_beneficiary_unpack_buffer_too_short", "[comm_msg]")
{
    uint8_t buf[100];
    buf[0] = COMM_MSG_LORA_SET_BENEFICIARY;

    comm_msg_set_beneficiary_t msg;
    TEST_ASSERT_EQUAL(-1, comm_msg_unpack_set_beneficiary(buf, sizeof(buf), &msg));
}

/**
 * Test : unpack avec params NULL -> erreur.
 */
TEST_CASE("set_beneficiary_unpack_null_params", "[comm_msg]")
{
    uint8_t buf[COMM_MSG_SET_BENEFICIARY_SIZE];
    buf[0] = COMM_MSG_LORA_SET_BENEFICIARY;
    comm_msg_set_beneficiary_t msg;

    TEST_ASSERT_EQUAL(-1, comm_msg_unpack_set_beneficiary(NULL, sizeof(buf), &msg));
    TEST_ASSERT_EQUAL(-1, comm_msg_unpack_set_beneficiary(buf, sizeof(buf), NULL));
}

/**
 * Test : comm_msg_get_type reconnait LORA_SET_BENEFICIARY.
 */
TEST_CASE("set_beneficiary_get_type", "[comm_msg]")
{
    uint8_t buf[1] = { COMM_MSG_LORA_SET_BENEFICIARY };
    comm_msg_type_t type;

    TEST_ASSERT_EQUAL(0, comm_msg_get_type(buf, 1, &type));
    TEST_ASSERT_EQUAL(COMM_MSG_LORA_SET_BENEFICIARY, type);
}

/* ================================================================
 * Tests verify_set_beneficiary [Lot B item 4]
 *
 * Canonical signed buffer :
 *   [target_key:32][beneficiary_key:32][interval:2 BE]
 * Signe par la cle privee du maitre (msg->master_key).
 * ================================================================ */

/**
 * Helper big-endian (duplique pour ne pas dependre des internes du
 * module : write_u16_be est static dans comm_msg.c).
 */
static void put_u16_be(uint8_t *dst, uint16_t v)
{
    dst[0] = (uint8_t)(v >> 8);
    dst[1] = (uint8_t)(v & 0xFF);
}

/**
 * Helper : signe (target_key, beneficiary_key, interval) avec la cle
 * privee du maitre, remplit msg.
 */
static void build_signed_set_beneficiary(const keypair_t *master_kp,
                                          const public_key_t *target_key,
                                          const public_key_t *beneficiary_key,
                                          uint16_t interval,
                                          comm_msg_set_beneficiary_t *out_msg)
{
    /* Canonical : [target:32][benef:32][interval:2 BE] */
    uint8_t signed_buf[CRYPTO_PUBLIC_KEY_SIZE * 2 + 2];
    size_t  signed_len = 0;
    memcpy(&signed_buf[signed_len], target_key->bytes, CRYPTO_PUBLIC_KEY_SIZE);
    signed_len += CRYPTO_PUBLIC_KEY_SIZE;
    memcpy(&signed_buf[signed_len], beneficiary_key->bytes,
           CRYPTO_PUBLIC_KEY_SIZE);
    signed_len += CRYPTO_PUBLIC_KEY_SIZE;
    put_u16_be(&signed_buf[signed_len], interval);
    signed_len += 2;

    signature_t sig;
    TEST_ASSERT_EQUAL(ESP_OK,
        crypto_sign(signed_buf, signed_len, master_kp, &sig));

    memcpy(&out_msg->master_key, &master_kp->public_key, sizeof(public_key_t));
    memcpy(&out_msg->signature, &sig, sizeof(signature_t));
    memcpy(&out_msg->target_key, target_key, sizeof(public_key_t));
    memcpy(&out_msg->beneficiary_key, beneficiary_key, sizeof(public_key_t));
    out_msg->forward_interval_min = interval;
}

/**
 * Cas nominal : message correctement signe par le maitre.
 */
TEST_CASE("set_beneficiary_verify_signature_valide", "[comm_msg]")
{
    TEST_ASSERT_EQUAL(ESP_OK, crypto_init());

    keypair_t master_kp;
    keypair_t target_kp;
    keypair_t beneficiary_kp;
    crypto_generate_keypair(&master_kp);
    crypto_generate_keypair(&target_kp);
    crypto_generate_keypair(&beneficiary_kp);

    comm_msg_set_beneficiary_t msg;
    build_signed_set_beneficiary(&master_kp,
                                  &target_kp.public_key,
                                  &beneficiary_kp.public_key,
                                  60, &msg);

    TEST_ASSERT_EQUAL(0, comm_msg_verify_set_beneficiary(&msg));
}

/**
 * Cas particulier : beneficiary_key = all-zeros (desactivation).
 * La signature doit etre valide quand meme.
 */
TEST_CASE("set_beneficiary_verify_desactivation", "[comm_msg]")
{
    TEST_ASSERT_EQUAL(ESP_OK, crypto_init());

    keypair_t master_kp;
    keypair_t target_kp;
    crypto_generate_keypair(&master_kp);
    crypto_generate_keypair(&target_kp);

    public_key_t zero_key;
    memset(zero_key.bytes, 0, CRYPTO_PUBLIC_KEY_SIZE);

    comm_msg_set_beneficiary_t msg;
    build_signed_set_beneficiary(&master_kp,
                                  &target_kp.public_key,
                                  &zero_key, 0, &msg);

    TEST_ASSERT_EQUAL(0, comm_msg_verify_set_beneficiary(&msg));
}

/**
 * Signature pourrie : rejet.
 */
TEST_CASE("set_beneficiary_verify_signature_invalide", "[comm_msg]")
{
    TEST_ASSERT_EQUAL(ESP_OK, crypto_init());

    keypair_t master_kp;
    keypair_t target_kp;
    keypair_t beneficiary_kp;
    crypto_generate_keypair(&master_kp);
    crypto_generate_keypair(&target_kp);
    crypto_generate_keypair(&beneficiary_kp);

    comm_msg_set_beneficiary_t msg = {0};
    memcpy(&msg.master_key, &master_kp.public_key, sizeof(public_key_t));
    fill_signature(&msg.signature, 0xFF);
    memcpy(&msg.target_key, &target_kp.public_key, sizeof(public_key_t));
    memcpy(&msg.beneficiary_key, &beneficiary_kp.public_key,
           sizeof(public_key_t));
    msg.forward_interval_min = 30;

    TEST_ASSERT_EQUAL(-1, comm_msg_verify_set_beneficiary(&msg));
}

/**
 * Alteration de l'interval apres signature : rejet attendu (l'interval
 * fait partie des donnees signees).
 */
TEST_CASE("set_beneficiary_verify_alteration_interval", "[comm_msg]")
{
    TEST_ASSERT_EQUAL(ESP_OK, crypto_init());

    keypair_t master_kp;
    keypair_t target_kp;
    keypair_t beneficiary_kp;
    crypto_generate_keypair(&master_kp);
    crypto_generate_keypair(&target_kp);
    crypto_generate_keypair(&beneficiary_kp);

    comm_msg_set_beneficiary_t msg;
    build_signed_set_beneficiary(&master_kp,
                                  &target_kp.public_key,
                                  &beneficiary_kp.public_key,
                                  60, &msg);
    TEST_ASSERT_EQUAL(0, comm_msg_verify_set_beneficiary(&msg));

    /* Changer l'interval sans resigner → la verif doit echouer. */
    msg.forward_interval_min = 5;
    TEST_ASSERT_EQUAL(-1, comm_msg_verify_set_beneficiary(&msg));
}

/**
 * Alteration de beneficiary_key apres signature : rejet attendu.
 *
 * Cas critique : un attaquant qui rerouterait le forward vers sa propre
 * cle en modifiant ce champ doit etre detecte par la verification.
 */
TEST_CASE("set_beneficiary_verify_alteration_beneficiary", "[comm_msg]")
{
    TEST_ASSERT_EQUAL(ESP_OK, crypto_init());

    keypair_t master_kp;
    keypair_t target_kp;
    keypair_t legit_benef;
    keypair_t attacker_kp;
    crypto_generate_keypair(&master_kp);
    crypto_generate_keypair(&target_kp);
    crypto_generate_keypair(&legit_benef);
    crypto_generate_keypair(&attacker_kp);

    comm_msg_set_beneficiary_t msg;
    build_signed_set_beneficiary(&master_kp,
                                  &target_kp.public_key,
                                  &legit_benef.public_key,
                                  60, &msg);
    TEST_ASSERT_EQUAL(0, comm_msg_verify_set_beneficiary(&msg));

    /* Detournement : remplacer le beneficiaire par l'attaquant. */
    memcpy(&msg.beneficiary_key, &attacker_kp.public_key,
           sizeof(public_key_t));
    TEST_ASSERT_EQUAL(-1, comm_msg_verify_set_beneficiary(&msg));
}

/**
 * Cas degenere : msg NULL.
 */
TEST_CASE("set_beneficiary_verify_msg_null", "[comm_msg]")
{
    TEST_ASSERT_EQUAL(-1, comm_msg_verify_set_beneficiary(NULL));
}
