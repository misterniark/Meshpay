/**
 * @file test_comm_msg_broadcast.c
 * @brief Tests unitaires du pack/unpack des messages LORA_BROADCAST.
 *
 * Vérifie le round-trip, les cas limites (texte max, texte min),
 * et les cas d'erreur (buffer trop court, texte trop long, NULL).
 */

#include "unity.h"
#include "comm/comm_msg.h"
#include "crypto/crypto_init.h"
#include "crypto/crypto_keys.h"
#include "crypto/crypto_sign.h"
#include <string.h>

void setUp(void) {}
void tearDown(void) {}

/* ================================================================
 * Helpers — données de test
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
 * Test : pack broadcast normal — vérifie le format wire.
 * Format attendu : [0x13][pubkey:32][sig:64][text_len:1][text:N]
 */
TEST_CASE("broadcast_pack_basic", "[comm_msg]")
{
    uint8_t buf[COMM_MSG_LORA_MAX];
    size_t out_len = 0;

    public_key_t key;
    fill_pubkey(&key, 0xAA);

    signature_t sig;
    fill_signature(&sig, 0xBB);

    const char *text = "Hello LoRa";
    uint8_t text_len = (uint8_t)strlen(text);

    int ret = comm_msg_pack_broadcast(buf, sizeof(buf),
                                      &key, &sig, text, text_len, &out_len);
    TEST_ASSERT_EQUAL(0, ret);

    /* Taille : 1 + 32 + 64 + 1 + 10 = 108 */
    TEST_ASSERT_EQUAL(COMM_MSG_BROADCAST_MIN_SIZE + text_len, out_len);

    /* Vérifier le type */
    TEST_ASSERT_EQUAL(COMM_MSG_LORA_BROADCAST, buf[0]);

    /* Vérifier la pubkey (octets 1..32) */
    TEST_ASSERT_EQUAL_MEMORY(key.bytes, &buf[1], CRYPTO_PUBLIC_KEY_SIZE);

    /* Vérifier la signature (octets 33..96) */
    TEST_ASSERT_EQUAL_MEMORY(sig.bytes, &buf[33], CRYPTO_SIGNATURE_SIZE);

    /* Vérifier text_len (octet 97) */
    TEST_ASSERT_EQUAL(text_len, buf[97]);

    /* Vérifier le texte (octets 98..107) */
    TEST_ASSERT_EQUAL_MEMORY(text, &buf[98], text_len);
}

/**
 * Test : pack avec texte de taille max (157 chars).
 */
TEST_CASE("broadcast_pack_max_text", "[comm_msg]")
{
    uint8_t buf[COMM_MSG_LORA_MAX];
    size_t out_len = 0;

    public_key_t key;
    fill_pubkey(&key, 0x11);

    signature_t sig;
    fill_signature(&sig, 0x22);

    /* Remplir un texte de 157 chars */
    char text[COMM_MSG_BROADCAST_TEXT_MAX + 1];
    memset(text, 'X', COMM_MSG_BROADCAST_TEXT_MAX);
    text[COMM_MSG_BROADCAST_TEXT_MAX] = '\0';

    int ret = comm_msg_pack_broadcast(buf, sizeof(buf),
                                      &key, &sig, text,
                                      COMM_MSG_BROADCAST_TEXT_MAX, &out_len);
    TEST_ASSERT_EQUAL(0, ret);

    /* Taille max : 98 + 157 = 255 (pile la limite LoRa) */
    TEST_ASSERT_EQUAL(COMM_MSG_LORA_MAX, out_len);
}

/**
 * Test : pack avec texte trop long (158 chars) → erreur.
 */
TEST_CASE("broadcast_pack_text_too_long", "[comm_msg]")
{
    uint8_t buf[COMM_MSG_LORA_MAX];
    size_t out_len = 0;

    public_key_t key;
    fill_pubkey(&key, 0x11);

    signature_t sig;
    fill_signature(&sig, 0x22);

    char text[200];
    memset(text, 'X', 200);

    /* text_len > COMM_MSG_BROADCAST_TEXT_MAX → erreur */
    int ret = comm_msg_pack_broadcast(buf, sizeof(buf),
                                      &key, &sig, text,
                                      COMM_MSG_BROADCAST_TEXT_MAX + 1, &out_len);
    TEST_ASSERT_EQUAL(-1, ret);
}

/**
 * Test : pack avec texte vide (text_len=0) → erreur.
 */
TEST_CASE("broadcast_pack_empty_text", "[comm_msg]")
{
    uint8_t buf[COMM_MSG_LORA_MAX];
    size_t out_len = 0;

    public_key_t key;
    fill_pubkey(&key, 0x11);

    signature_t sig;
    fill_signature(&sig, 0x22);

    int ret = comm_msg_pack_broadcast(buf, sizeof(buf),
                                      &key, &sig, "hello", 0, &out_len);
    TEST_ASSERT_EQUAL(-1, ret);
}

/**
 * Test : pack avec buffer trop petit → erreur.
 */
TEST_CASE("broadcast_pack_buffer_too_small", "[comm_msg]")
{
    uint8_t buf[50]; /* Bien trop petit pour un broadcast (min 99) */
    size_t out_len = 0;

    public_key_t key;
    fill_pubkey(&key, 0x11);

    signature_t sig;
    fill_signature(&sig, 0x22);

    int ret = comm_msg_pack_broadcast(buf, sizeof(buf),
                                      &key, &sig, "Hi", 2, &out_len);
    TEST_ASSERT_EQUAL(-1, ret);
}

/**
 * Test : pack avec paramètres NULL → erreur.
 */
TEST_CASE("broadcast_pack_null_params", "[comm_msg]")
{
    uint8_t buf[COMM_MSG_LORA_MAX];
    size_t out_len = 0;
    public_key_t key;
    signature_t sig;

    TEST_ASSERT_EQUAL(-1, comm_msg_pack_broadcast(NULL, sizeof(buf),
                                                   &key, &sig, "a", 1, &out_len));
    TEST_ASSERT_EQUAL(-1, comm_msg_pack_broadcast(buf, sizeof(buf),
                                                   NULL, &sig, "a", 1, &out_len));
    TEST_ASSERT_EQUAL(-1, comm_msg_pack_broadcast(buf, sizeof(buf),
                                                   &key, NULL, "a", 1, &out_len));
    TEST_ASSERT_EQUAL(-1, comm_msg_pack_broadcast(buf, sizeof(buf),
                                                   &key, &sig, NULL, 1, &out_len));
    TEST_ASSERT_EQUAL(-1, comm_msg_pack_broadcast(buf, sizeof(buf),
                                                   &key, &sig, "a", 1, NULL));
}

/* ================================================================
 * Tests de unpack
 * ================================================================ */

/**
 * Test : round-trip pack → unpack — le contenu doit être identique.
 */
TEST_CASE("broadcast_roundtrip", "[comm_msg]")
{
    uint8_t buf[COMM_MSG_LORA_MAX];
    size_t out_len = 0;

    public_key_t key;
    fill_pubkey(&key, 0xCC);

    signature_t sig;
    fill_signature(&sig, 0xDD);

    const char *text = "Fermeture a 18h";
    uint8_t text_len = (uint8_t)strlen(text);

    /* Pack */
    TEST_ASSERT_EQUAL(0, comm_msg_pack_broadcast(buf, sizeof(buf),
                                                  &key, &sig, text, text_len,
                                                  &out_len));

    /* Unpack */
    comm_msg_broadcast_t msg;
    memset(&msg, 0, sizeof(msg));
    TEST_ASSERT_EQUAL(0, comm_msg_unpack_broadcast(buf, out_len, &msg));

    /* Vérifier que tout est préservé */
    TEST_ASSERT_EQUAL_MEMORY(key.bytes, msg.sender_key.bytes, CRYPTO_PUBLIC_KEY_SIZE);
    TEST_ASSERT_EQUAL_MEMORY(sig.bytes, msg.signature.bytes, CRYPTO_SIGNATURE_SIZE);
    TEST_ASSERT_EQUAL(text_len, msg.text_len);
    TEST_ASSERT_EQUAL_STRING(text, msg.text);
}

/**
 * Test : round-trip avec texte max (157 chars).
 */
TEST_CASE("broadcast_roundtrip_max", "[comm_msg]")
{
    uint8_t buf[COMM_MSG_LORA_MAX];
    size_t out_len = 0;

    public_key_t key;
    fill_pubkey(&key, 0x55);

    signature_t sig;
    fill_signature(&sig, 0x66);

    char text[COMM_MSG_BROADCAST_TEXT_MAX + 1];
    memset(text, 'Z', COMM_MSG_BROADCAST_TEXT_MAX);
    text[COMM_MSG_BROADCAST_TEXT_MAX] = '\0';

    TEST_ASSERT_EQUAL(0, comm_msg_pack_broadcast(buf, sizeof(buf),
                                                  &key, &sig, text,
                                                  COMM_MSG_BROADCAST_TEXT_MAX,
                                                  &out_len));

    comm_msg_broadcast_t msg;
    TEST_ASSERT_EQUAL(0, comm_msg_unpack_broadcast(buf, out_len, &msg));

    TEST_ASSERT_EQUAL(COMM_MSG_BROADCAST_TEXT_MAX, msg.text_len);
    TEST_ASSERT_EQUAL_MEMORY(text, msg.text, COMM_MSG_BROADCAST_TEXT_MAX);
    TEST_ASSERT_EQUAL('\0', msg.text[COMM_MSG_BROADCAST_TEXT_MAX]);
}

/**
 * Test : unpack avec buffer trop court → erreur.
 */
TEST_CASE("broadcast_unpack_buffer_too_short", "[comm_msg]")
{
    /* Buffer de 50 octets — en dessous du minimum de 98 */
    uint8_t buf[50];
    buf[0] = COMM_MSG_LORA_BROADCAST;

    comm_msg_broadcast_t msg;
    TEST_ASSERT_EQUAL(-1, comm_msg_unpack_broadcast(buf, sizeof(buf), &msg));
}

/**
 * Test : unpack avec mauvais type → erreur.
 */
TEST_CASE("broadcast_unpack_wrong_type", "[comm_msg]")
{
    uint8_t buf[COMM_MSG_LORA_MAX];
    memset(buf, 0, sizeof(buf));
    buf[0] = COMM_MSG_LORA_TX; /* Mauvais type */

    comm_msg_broadcast_t msg;
    TEST_ASSERT_EQUAL(-1, comm_msg_unpack_broadcast(buf, sizeof(buf), &msg));
}

/**
 * Test : unpack avec text_len qui depasse la fin du buffer → erreur.
 */
TEST_CASE("broadcast_unpack_text_exceeds_buffer", "[comm_msg]")
{
    uint8_t buf[COMM_MSG_LORA_MAX];
    size_t out_len = 0;

    public_key_t key;
    fill_pubkey(&key, 0x11);

    signature_t sig;
    fill_signature(&sig, 0x22);

    /* Pack un message de 10 chars */
    comm_msg_pack_broadcast(buf, sizeof(buf),
                            &key, &sig, "0123456789", 10, &out_len);

    /* Tronquer le buffer pour que le texte dépasse */
    comm_msg_broadcast_t msg;
    TEST_ASSERT_EQUAL(-1, comm_msg_unpack_broadcast(buf, COMM_MSG_BROADCAST_MIN_SIZE + 5, &msg));
}

/**
 * Test : comm_msg_get_type reconnaît LORA_BROADCAST.
 */
TEST_CASE("broadcast_get_type", "[comm_msg]")
{
    uint8_t buf[1] = { COMM_MSG_LORA_BROADCAST };
    comm_msg_type_t type;

    TEST_ASSERT_EQUAL(0, comm_msg_get_type(buf, 1, &type));
    TEST_ASSERT_EQUAL(COMM_MSG_LORA_BROADCAST, type);
}

/* ================================================================
 * Tests verify_broadcast [Lot B item 4]
 *
 * Verifient la coherence cryptographique du message decode contre
 * sa cle publique d'emetteur. Le canonical signed buffer est
 * [text_len:1][text:N] (cf. comm_msg_pack_broadcast).
 * ================================================================ */

/**
 * Helper : signe un texte de broadcast avec la cle privee fournie
 * et remplit msg avec les champs cryptographiques + le texte.
 */
static void build_signed_broadcast(const keypair_t *kp,
                                    const char *text, uint8_t text_len,
                                    comm_msg_broadcast_t *out_msg)
{
    /* Canonical signed buffer : [text_len:1][text:N] */
    uint8_t signed_buf[1 + COMM_MSG_BROADCAST_TEXT_MAX];
    signed_buf[0] = text_len;
    memcpy(&signed_buf[1], text, text_len);

    signature_t sig;
    TEST_ASSERT_EQUAL(ESP_OK,
        crypto_sign(signed_buf, (size_t)(1 + text_len), kp, &sig));

    memcpy(&out_msg->sender_key, &kp->public_key, sizeof(public_key_t));
    memcpy(&out_msg->signature, &sig, sizeof(signature_t));
    out_msg->text_len = text_len;
    memcpy(out_msg->text, text, text_len);
    out_msg->text[text_len] = '\0';
}

/**
 * Cas nominal : un broadcast correctement signe doit etre accepte.
 */
TEST_CASE("broadcast_verify_signature_valide", "[comm_msg]")
{
    TEST_ASSERT_EQUAL(ESP_OK, crypto_init());

    keypair_t kp;
    TEST_ASSERT_EQUAL(ESP_OK, crypto_generate_keypair(&kp));

    comm_msg_broadcast_t msg;
    build_signed_broadcast(&kp, "Bonjour reseau", 14, &msg);

    TEST_ASSERT_EQUAL(0, comm_msg_verify_broadcast(&msg));
}

/**
 * Signature pourrie : un buffer rempli de patterns (cf. anciens tests)
 * ne doit PAS etre accepte comme signature valide.
 */
TEST_CASE("broadcast_verify_signature_invalide", "[comm_msg]")
{
    TEST_ASSERT_EQUAL(ESP_OK, crypto_init());

    keypair_t kp;
    crypto_generate_keypair(&kp);

    comm_msg_broadcast_t msg;
    memcpy(&msg.sender_key, &kp.public_key, sizeof(public_key_t));
    fill_signature(&msg.signature, 0xFF); /* garbage */
    const char *text = "Hello";
    msg.text_len = (uint8_t)strlen(text);
    memcpy(msg.text, text, msg.text_len);
    msg.text[msg.text_len] = '\0';

    TEST_ASSERT_EQUAL(-1, comm_msg_verify_broadcast(&msg));
}

/**
 * Alteration du texte apres signature : la verification doit echouer.
 *
 * Couvre la propriete d'integrite : si un attaquant intercepte et modifie
 * un seul octet du texte, le receveur le detecte.
 */
TEST_CASE("broadcast_verify_alteration_texte_detectee", "[comm_msg]")
{
    TEST_ASSERT_EQUAL(ESP_OK, crypto_init());

    keypair_t kp;
    crypto_generate_keypair(&kp);

    comm_msg_broadcast_t msg;
    build_signed_broadcast(&kp, "Texte d'origine", 15, &msg);

    /* Sanity : signature valide avant alteration */
    TEST_ASSERT_EQUAL(0, comm_msg_verify_broadcast(&msg));

    /* Alterer un octet du texte */
    msg.text[0] ^= 0xFF;
    TEST_ASSERT_EQUAL(-1, comm_msg_verify_broadcast(&msg));
}

/**
 * Signature valide sous une AUTRE cle : rejet attendu.
 */
TEST_CASE("broadcast_verify_mauvais_signataire", "[comm_msg]")
{
    TEST_ASSERT_EQUAL(ESP_OK, crypto_init());

    keypair_t kp_signer;
    keypair_t kp_impostor;
    crypto_generate_keypair(&kp_signer);
    crypto_generate_keypair(&kp_impostor);

    comm_msg_broadcast_t msg;
    build_signed_broadcast(&kp_signer, "Hello", 5, &msg);

    /* Remplacer sender_key par celle d'un autre device : la signature
       de kp_signer ne verifiera pas contre kp_impostor.public_key. */
    memcpy(&msg.sender_key, &kp_impostor.public_key, sizeof(public_key_t));

    TEST_ASSERT_EQUAL(-1, comm_msg_verify_broadcast(&msg));
}

/**
 * Cas degenere : msg NULL.
 */
TEST_CASE("broadcast_verify_msg_null", "[comm_msg]")
{
    TEST_ASSERT_EQUAL(-1, comm_msg_verify_broadcast(NULL));
}

/**
 * text_len hors bornes : 0 ou > COMM_MSG_BROADCAST_TEXT_MAX doivent
 * etre refuses sans appel a crypto_verify.
 */
TEST_CASE("broadcast_verify_text_len_hors_bornes", "[comm_msg]")
{
    comm_msg_broadcast_t msg = {0};
    msg.text_len = 0;
    TEST_ASSERT_EQUAL(-1, comm_msg_verify_broadcast(&msg));

    msg.text_len = COMM_MSG_BROADCAST_TEXT_MAX + 1;
    TEST_ASSERT_EQUAL(-1, comm_msg_verify_broadcast(&msg));
}
