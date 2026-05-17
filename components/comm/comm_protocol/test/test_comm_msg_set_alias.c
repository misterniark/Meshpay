/**
 * @file test_comm_msg_set_alias.c
 * @brief Tests unitaires du pack/unpack des messages LORA_SET_ALIAS.
 *
 * Verifie le round-trip, les cas limites (alias max, alias vide),
 * et les cas d'erreur (buffer trop court, type incorrect, NULL, alias trop long).
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
 * Tests SET_ALIAS — Pack
 * ================================================================ */

/**
 * Test : pack SET_ALIAS — verifie le format wire.
 * Format : [0x16][master_key:32][sig:64][target_key:32][alias_len:1][alias:N]
 */
TEST_CASE("set_alias_pack_basic", "[comm_msg]")
{
    uint8_t buf[COMM_MSG_LORA_MAX];
    size_t out_len = 0;

    public_key_t master_key, target_key;
    fill_pubkey(&master_key, 0xAA);
    fill_pubkey(&target_key, 0xBB);

    signature_t sig;
    fill_signature(&sig, 0xCC);

    const char *alias = "Brave-Loup";
    uint8_t alias_len = (uint8_t)strlen(alias);

    int ret = comm_msg_pack_set_alias(buf, sizeof(buf),
                                       &master_key, &sig, &target_key,
                                       alias, alias_len, &out_len);
    TEST_ASSERT_EQUAL(0, ret);
    TEST_ASSERT_EQUAL(COMM_MSG_SET_ALIAS_MIN_SIZE + alias_len, out_len);

    /* Verifier le type */
    TEST_ASSERT_EQUAL(COMM_MSG_LORA_SET_ALIAS, buf[0]);

    /* Verifier la master_key */
    TEST_ASSERT_EQUAL_MEMORY(master_key.bytes, &buf[1], CRYPTO_PUBLIC_KEY_SIZE);

    /* Verifier la signature */
    TEST_ASSERT_EQUAL_MEMORY(sig.bytes, &buf[33], CRYPTO_SIGNATURE_SIZE);

    /* Verifier la target_key */
    TEST_ASSERT_EQUAL_MEMORY(target_key.bytes, &buf[97], CRYPTO_PUBLIC_KEY_SIZE);

    /* Verifier alias_len + alias */
    TEST_ASSERT_EQUAL(alias_len, buf[129]);
    TEST_ASSERT_EQUAL_MEMORY(alias, &buf[130], alias_len);
}

/* ================================================================
 * Tests SET_ALIAS — Round-trip
 * ================================================================ */

/**
 * Test : round-trip pack -> unpack SET_ALIAS.
 */
TEST_CASE("set_alias_roundtrip", "[comm_msg]")
{
    uint8_t buf[COMM_MSG_LORA_MAX];
    size_t out_len = 0;

    public_key_t master_key, target_key;
    fill_pubkey(&master_key, 0x11);
    fill_pubkey(&target_key, 0x22);

    signature_t sig;
    fill_signature(&sig, 0x33);

    const char *alias = "Vif-Renard";
    uint8_t alias_len = (uint8_t)strlen(alias);

    TEST_ASSERT_EQUAL(0, comm_msg_pack_set_alias(buf, sizeof(buf),
                                                  &master_key, &sig,
                                                  &target_key,
                                                  alias, alias_len, &out_len));

    comm_msg_set_alias_t msg;
    memset(&msg, 0, sizeof(msg));
    TEST_ASSERT_EQUAL(0, comm_msg_unpack_set_alias(buf, out_len, &msg));

    TEST_ASSERT_EQUAL_MEMORY(master_key.bytes, msg.master_key.bytes, CRYPTO_PUBLIC_KEY_SIZE);
    TEST_ASSERT_EQUAL_MEMORY(sig.bytes, msg.signature.bytes, CRYPTO_SIGNATURE_SIZE);
    TEST_ASSERT_EQUAL_MEMORY(target_key.bytes, msg.target_key.bytes, CRYPTO_PUBLIC_KEY_SIZE);
    TEST_ASSERT_EQUAL(alias_len, msg.alias_len);
    TEST_ASSERT_EQUAL_STRING(alias, msg.alias);
}

/**
 * Test : round-trip SET_ALIAS avec alias max (32 chars).
 */
TEST_CASE("set_alias_roundtrip_max_alias", "[comm_msg]")
{
    uint8_t buf[COMM_MSG_LORA_MAX];
    size_t out_len = 0;

    public_key_t master_key, target_key;
    fill_pubkey(&master_key, 0x44);
    fill_pubkey(&target_key, 0x55);

    signature_t sig;
    fill_signature(&sig, 0x66);

    char alias[COMM_MSG_ALIAS_MAX + 1];
    memset(alias, 'Z', COMM_MSG_ALIAS_MAX);
    alias[COMM_MSG_ALIAS_MAX] = '\0';

    TEST_ASSERT_EQUAL(0, comm_msg_pack_set_alias(buf, sizeof(buf),
                                                  &master_key, &sig,
                                                  &target_key,
                                                  alias, COMM_MSG_ALIAS_MAX,
                                                  &out_len));

    comm_msg_set_alias_t msg;
    TEST_ASSERT_EQUAL(0, comm_msg_unpack_set_alias(buf, out_len, &msg));

    TEST_ASSERT_EQUAL(COMM_MSG_ALIAS_MAX, msg.alias_len);
    TEST_ASSERT_EQUAL_MEMORY(alias, msg.alias, COMM_MSG_ALIAS_MAX);
}

/* ================================================================
 * Tests SET_ALIAS — Erreurs
 * ================================================================ */

/**
 * Test : pack SET_ALIAS avec alias vide (len=0) -> erreur.
 * Un alias vide n'a pas de sens pour un renommage.
 */
TEST_CASE("set_alias_pack_empty_alias", "[comm_msg]")
{
    uint8_t buf[COMM_MSG_LORA_MAX];
    size_t out_len = 0;

    public_key_t master_key, target_key;
    fill_pubkey(&master_key, 0x77);
    fill_pubkey(&target_key, 0x88);

    signature_t sig;
    fill_signature(&sig, 0x99);

    TEST_ASSERT_EQUAL(-1, comm_msg_pack_set_alias(buf, sizeof(buf),
                                                   &master_key, &sig,
                                                   &target_key,
                                                   "", 0, &out_len));
}

/**
 * Test : pack SET_ALIAS avec alias trop long -> erreur.
 */
TEST_CASE("set_alias_pack_alias_too_long", "[comm_msg]")
{
    uint8_t buf[COMM_MSG_LORA_MAX];
    size_t out_len = 0;

    public_key_t master_key, target_key;
    fill_pubkey(&master_key, 0xAA);
    fill_pubkey(&target_key, 0xBB);

    signature_t sig;
    fill_signature(&sig, 0xCC);

    char alias[COMM_MSG_ALIAS_MAX + 10];
    memset(alias, 'X', sizeof(alias) - 1);
    alias[sizeof(alias) - 1] = '\0';

    TEST_ASSERT_EQUAL(-1, comm_msg_pack_set_alias(buf, sizeof(buf),
                                                   &master_key, &sig,
                                                   &target_key,
                                                   alias, COMM_MSG_ALIAS_MAX + 1,
                                                   &out_len));
}

/**
 * Test : pack SET_ALIAS avec buffer trop petit -> erreur.
 */
TEST_CASE("set_alias_pack_buffer_too_small", "[comm_msg]")
{
    uint8_t buf[50]; /* Bien trop petit pour 130 + N */
    size_t out_len = 0;

    public_key_t master_key, target_key;
    fill_pubkey(&master_key, 0x11);
    fill_pubkey(&target_key, 0x22);

    signature_t sig;
    fill_signature(&sig, 0x33);

    TEST_ASSERT_EQUAL(-1, comm_msg_pack_set_alias(buf, sizeof(buf),
                                                   &master_key, &sig,
                                                   &target_key,
                                                   "Test", 4, &out_len));
}

/**
 * Test : pack SET_ALIAS avec params NULL -> erreur.
 */
TEST_CASE("set_alias_pack_null_params", "[comm_msg]")
{
    uint8_t buf[COMM_MSG_LORA_MAX];
    size_t out_len = 0;
    public_key_t key = {0};
    signature_t sig = {0};

    TEST_ASSERT_EQUAL(-1, comm_msg_pack_set_alias(NULL, sizeof(buf), &key, &sig, &key, "a", 1, &out_len));
    TEST_ASSERT_EQUAL(-1, comm_msg_pack_set_alias(buf, sizeof(buf), NULL, &sig, &key, "a", 1, &out_len));
    TEST_ASSERT_EQUAL(-1, comm_msg_pack_set_alias(buf, sizeof(buf), &key, NULL, &key, "a", 1, &out_len));
    TEST_ASSERT_EQUAL(-1, comm_msg_pack_set_alias(buf, sizeof(buf), &key, &sig, NULL, "a", 1, &out_len));
    TEST_ASSERT_EQUAL(-1, comm_msg_pack_set_alias(buf, sizeof(buf), &key, &sig, &key, NULL, 1, &out_len));
    TEST_ASSERT_EQUAL(-1, comm_msg_pack_set_alias(buf, sizeof(buf), &key, &sig, &key, "a", 1, NULL));
}

/**
 * Test : unpack SET_ALIAS avec mauvais type -> erreur.
 */
TEST_CASE("set_alias_unpack_wrong_type", "[comm_msg]")
{
    uint8_t buf[COMM_MSG_SET_ALIAS_MIN_SIZE + 4];
    memset(buf, 0, sizeof(buf));
    buf[0] = COMM_MSG_LORA_PING; /* Mauvais type */
    buf[129] = 4; /* alias_len */

    comm_msg_set_alias_t msg;
    TEST_ASSERT_EQUAL(-1, comm_msg_unpack_set_alias(buf, sizeof(buf), &msg));
}

/**
 * Test : unpack SET_ALIAS avec buffer trop court -> erreur.
 */
TEST_CASE("set_alias_unpack_buffer_too_short", "[comm_msg]")
{
    uint8_t buf[50];
    buf[0] = COMM_MSG_LORA_SET_ALIAS;

    comm_msg_set_alias_t msg;
    TEST_ASSERT_EQUAL(-1, comm_msg_unpack_set_alias(buf, sizeof(buf), &msg));
}

/**
 * Test : unpack SET_ALIAS avec alias qui depasse le buffer -> erreur.
 */
TEST_CASE("set_alias_unpack_alias_exceeds_buffer", "[comm_msg]")
{
    uint8_t buf[COMM_MSG_LORA_MAX];
    size_t out_len = 0;

    public_key_t master_key, target_key;
    fill_pubkey(&master_key, 0xDD);
    fill_pubkey(&target_key, 0xEE);

    signature_t sig;
    fill_signature(&sig, 0xFF);

    /* Pack un SET_ALIAS avec alias de 10 chars */
    comm_msg_pack_set_alias(buf, sizeof(buf),
                             &master_key, &sig, &target_key,
                             "0123456789", 10, &out_len);

    /* Tronquer le buffer pour que l'alias depasse */
    comm_msg_set_alias_t msg;
    TEST_ASSERT_EQUAL(-1, comm_msg_unpack_set_alias(buf, COMM_MSG_SET_ALIAS_MIN_SIZE + 5, &msg));
}

/**
 * Test : comm_msg_get_type reconnait LORA_SET_ALIAS.
 */
TEST_CASE("set_alias_get_type", "[comm_msg]")
{
    uint8_t buf[1] = { COMM_MSG_LORA_SET_ALIAS };
    comm_msg_type_t type;

    TEST_ASSERT_EQUAL(0, comm_msg_get_type(buf, 1, &type));
    TEST_ASSERT_EQUAL(COMM_MSG_LORA_SET_ALIAS, type);
}

/* ================================================================
 * Tests verify_set_alias [Lot B item 4]
 *
 * Canonical signed buffer : [target_key:32][alias_len:1][alias:N]
 * Signe par la cle privee du maitre (msg->master_key).
 * ================================================================ */

/**
 * Helper : signe le couple (target_key, alias) avec la cle privee du
 * maitre, remplit msg avec les champs requis.
 */
static void build_signed_set_alias(const keypair_t *master_kp,
                                    const public_key_t *target_key,
                                    const char *alias, uint8_t alias_len,
                                    comm_msg_set_alias_t *out_msg)
{
    /* Canonical signed buffer : [target_key:32][alias_len:1][alias:N] */
    uint8_t signed_buf[CRYPTO_PUBLIC_KEY_SIZE + 1 + COMM_MSG_ALIAS_MAX];
    size_t  signed_len = 0;
    memcpy(&signed_buf[signed_len], target_key->bytes, CRYPTO_PUBLIC_KEY_SIZE);
    signed_len += CRYPTO_PUBLIC_KEY_SIZE;
    signed_buf[signed_len++] = alias_len;
    memcpy(&signed_buf[signed_len], alias, alias_len);
    signed_len += alias_len;

    signature_t sig;
    TEST_ASSERT_EQUAL(ESP_OK,
        crypto_sign(signed_buf, signed_len, master_kp, &sig));

    memcpy(&out_msg->master_key, &master_kp->public_key, sizeof(public_key_t));
    memcpy(&out_msg->signature, &sig, sizeof(signature_t));
    memcpy(&out_msg->target_key, target_key, sizeof(public_key_t));
    out_msg->alias_len = alias_len;
    memcpy(out_msg->alias, alias, alias_len);
    out_msg->alias[alias_len] = '\0';
}

/**
 * Cas nominal : message signe par le maitre, target_key arbitraire.
 */
TEST_CASE("set_alias_verify_signature_valide", "[comm_msg]")
{
    TEST_ASSERT_EQUAL(ESP_OK, crypto_init());

    keypair_t master_kp;
    keypair_t target_kp;
    TEST_ASSERT_EQUAL(ESP_OK, crypto_generate_keypair(&master_kp));
    TEST_ASSERT_EQUAL(ESP_OK, crypto_generate_keypair(&target_kp));

    comm_msg_set_alias_t msg;
    build_signed_set_alias(&master_kp, &target_kp.public_key,
                            "Brave-Loup", 10, &msg);

    TEST_ASSERT_EQUAL(0, comm_msg_verify_set_alias(&msg));
}

/**
 * Signature pourrie : rejet attendu.
 */
TEST_CASE("set_alias_verify_signature_invalide", "[comm_msg]")
{
    TEST_ASSERT_EQUAL(ESP_OK, crypto_init());

    keypair_t master_kp;
    keypair_t target_kp;
    crypto_generate_keypair(&master_kp);
    crypto_generate_keypair(&target_kp);

    comm_msg_set_alias_t msg = {0};
    memcpy(&msg.master_key, &master_kp.public_key, sizeof(public_key_t));
    fill_signature(&msg.signature, 0xFF);
    memcpy(&msg.target_key, &target_kp.public_key, sizeof(public_key_t));
    const char *alias = "Fake-Cygne";
    msg.alias_len = (uint8_t)strlen(alias);
    memcpy(msg.alias, alias, msg.alias_len);

    TEST_ASSERT_EQUAL(-1, comm_msg_verify_set_alias(&msg));
}

/**
 * Alteration du target_key apres signature : la signature couvre target_key,
 * donc le changer doit faire echouer la verif.
 */
TEST_CASE("set_alias_verify_alteration_target_detectee", "[comm_msg]")
{
    TEST_ASSERT_EQUAL(ESP_OK, crypto_init());

    keypair_t master_kp;
    keypair_t target_kp;
    keypair_t other_kp;
    crypto_generate_keypair(&master_kp);
    crypto_generate_keypair(&target_kp);
    crypto_generate_keypair(&other_kp);

    comm_msg_set_alias_t msg;
    build_signed_set_alias(&master_kp, &target_kp.public_key,
                            "Alice", 5, &msg);
    TEST_ASSERT_EQUAL(0, comm_msg_verify_set_alias(&msg));

    /* Substituer target_key par autre cle → signature couvre l'ancien
       target, donc verif doit echouer. */
    memcpy(&msg.target_key, &other_kp.public_key, sizeof(public_key_t));
    TEST_ASSERT_EQUAL(-1, comm_msg_verify_set_alias(&msg));
}

/**
 * Alteration de l'alias apres signature : meme principe.
 */
TEST_CASE("set_alias_verify_alteration_alias_detectee", "[comm_msg]")
{
    TEST_ASSERT_EQUAL(ESP_OK, crypto_init());

    keypair_t master_kp;
    keypair_t target_kp;
    crypto_generate_keypair(&master_kp);
    crypto_generate_keypair(&target_kp);

    comm_msg_set_alias_t msg;
    build_signed_set_alias(&master_kp, &target_kp.public_key,
                            "Original", 8, &msg);
    TEST_ASSERT_EQUAL(0, comm_msg_verify_set_alias(&msg));

    msg.alias[0] ^= 0xFF;
    TEST_ASSERT_EQUAL(-1, comm_msg_verify_set_alias(&msg));
}

/**
 * Signature valide d'un autre maitre : un attaquant qui signe avec sa
 * propre cle ne doit pas etre accepte (verification contre master_key).
 */
TEST_CASE("set_alias_verify_mauvais_signataire", "[comm_msg]")
{
    TEST_ASSERT_EQUAL(ESP_OK, crypto_init());

    keypair_t real_master;
    keypair_t impostor;
    keypair_t target_kp;
    crypto_generate_keypair(&real_master);
    crypto_generate_keypair(&impostor);
    crypto_generate_keypair(&target_kp);

    comm_msg_set_alias_t msg;
    build_signed_set_alias(&impostor, &target_kp.public_key, "Hack", 4, &msg);

    /* Substituer master_key par celle du vrai maitre → la signature de
       l'impostor ne verifiera pas contre real_master.public_key. */
    memcpy(&msg.master_key, &real_master.public_key, sizeof(public_key_t));
    TEST_ASSERT_EQUAL(-1, comm_msg_verify_set_alias(&msg));
}

/**
 * Cas degeneres : msg NULL, alias_len hors bornes.
 */
TEST_CASE("set_alias_verify_cas_degeneres", "[comm_msg]")
{
    TEST_ASSERT_EQUAL(-1, comm_msg_verify_set_alias(NULL));

    comm_msg_set_alias_t msg = {0};
    msg.alias_len = 0;
    TEST_ASSERT_EQUAL(-1, comm_msg_verify_set_alias(&msg));

    msg.alias_len = COMM_MSG_ALIAS_MAX + 1;
    TEST_ASSERT_EQUAL(-1, comm_msg_verify_set_alias(&msg));
}
