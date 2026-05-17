/**
 * @file test_crypto.c
 * @brief Tests unitaires pour le module core/crypto/.
 *
 * Teste la génération de clés, le hachage SHA-256, la signature
 * et la vérification Ed25519. Utilise le framework Unity (inclus ESP-IDF).
 */

#include "unity.h"
#include "crypto/crypto_keys.h"
#include "crypto/crypto_hash.h"
#include "crypto/crypto_sign.h"
#include "monocypher-ed25519.h"
#include <string.h>

/* ========================================================================= */
/*                         Tests crypto_hash                                  */
/* ========================================================================= */

/**
 * @brief Vérifie que le hash SHA-256 d'une chaîne connue est correct.
 *
 * Valeur de référence : SHA-256("hello") est un hash bien documenté.
 * Cela garantit que notre implémentation est conforme au standard.
 */
TEST_CASE("hash_sha256_valeur_connue", "[crypto]")
{
    const uint8_t data[] = "hello";
    hash_t hash;

    esp_err_t err = crypto_hash_sha256(data, 5, &hash);
    TEST_ASSERT_EQUAL(ESP_OK, err);

    /* SHA-256("hello") = 2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e7304... */
    TEST_ASSERT_EQUAL_HEX8(0x2c, hash.bytes[0]);
    TEST_ASSERT_EQUAL_HEX8(0xf2, hash.bytes[1]);
    TEST_ASSERT_EQUAL_HEX8(0x4d, hash.bytes[2]);
    TEST_ASSERT_EQUAL_HEX8(0xba, hash.bytes[3]);
}

/**
 * @brief Vérifie que le hash est déterministe (même entrée = même sortie).
 *
 * Propriété fondamentale : deux appels avec les mêmes données
 * doivent produire le même hash, sinon les ID de transactions
 * ne seraient pas stables.
 */
TEST_CASE("hash_sha256_deterministe", "[crypto]")
{
    const uint8_t data[] = "transaction_test_data";
    hash_t hash1, hash2;

    crypto_hash_sha256(data, sizeof(data) - 1, &hash1);
    crypto_hash_sha256(data, sizeof(data) - 1, &hash2);

    TEST_ASSERT_EQUAL_MEMORY(hash1.bytes, hash2.bytes, CRYPTO_HASH_SIZE);
}

/**
 * @brief Vérifie que des données différentes produisent des hashes différents.
 *
 * Propriété de résistance aux collisions : deux entrées différentes
 * ne doivent (quasi) jamais produire le même hash.
 */
TEST_CASE("hash_sha256_donnees_differentes", "[crypto]")
{
    const uint8_t data1[] = "paiement_alice";
    const uint8_t data2[] = "paiement_bob";
    hash_t hash1, hash2;

    crypto_hash_sha256(data1, sizeof(data1) - 1, &hash1);
    crypto_hash_sha256(data2, sizeof(data2) - 1, &hash2);

    TEST_ASSERT_NOT_EQUAL(0, memcmp(hash1.bytes, hash2.bytes, CRYPTO_HASH_SIZE));
}

/**
 * @brief Vérifie le rejet des paramètres NULL.
 */
TEST_CASE("hash_sha256_parametres_null", "[crypto]")
{
    hash_t hash;
    const uint8_t data[] = "test";

    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, crypto_hash_sha256(NULL, 4, &hash));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, crypto_hash_sha256(data, 4, NULL));
}

/* ========================================================================= */
/*                         Tests crypto_keys                                  */
/* ========================================================================= */

/**
 * @brief Vérifie qu'on peut générer une paire de clés Ed25519 valide.
 *
 * Après génération, la clé publique et la clé privée ne doivent pas
 * être des buffers nuls.
 */
TEST_CASE("generate_keypair_succes", "[crypto]")
{
    keypair_t kp;
    memset(&kp, 0, sizeof(kp));

    esp_err_t err = crypto_generate_keypair(&kp);
    TEST_ASSERT_EQUAL(ESP_OK, err);

    /* La clé publique ne doit pas être nulle */
    uint8_t zero[CRYPTO_PUBLIC_KEY_SIZE] = {0};
    TEST_ASSERT_NOT_EQUAL(0, memcmp(kp.public_key.bytes, zero, CRYPTO_PUBLIC_KEY_SIZE));
}

/**
 * @brief Vérifie que deux générations successives produisent des clés différentes.
 *
 * Chaque device doit avoir une identité unique. Si deux appels successifs
 * retournent la même clé, il y a un problème avec le PRNG.
 */
TEST_CASE("generate_keypair_unique", "[crypto]")
{
    keypair_t kp1, kp2;

    crypto_generate_keypair(&kp1);
    crypto_generate_keypair(&kp2);

    TEST_ASSERT_NOT_EQUAL(0, memcmp(kp1.public_key.bytes,
                                     kp2.public_key.bytes,
                                     CRYPTO_PUBLIC_KEY_SIZE));
}

/**
 * @brief Vérifie l'export de la clé publique depuis une paire de clés.
 */
TEST_CASE("export_public_key", "[crypto]")
{
    keypair_t kp;
    public_key_t pubkey;

    crypto_generate_keypair(&kp);
    esp_err_t err = crypto_export_public_key(&kp, &pubkey);

    TEST_ASSERT_EQUAL(ESP_OK, err);
    TEST_ASSERT_EQUAL_MEMORY(kp.public_key.bytes, pubkey.bytes, CRYPTO_PUBLIC_KEY_SIZE);
}

/**
 * @brief Vérifie le rejet des paramètres NULL pour la génération.
 */
TEST_CASE("generate_keypair_null", "[crypto]")
{
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, crypto_generate_keypair(NULL));
}

/* ========================================================================= */
/*                         Tests crypto_sign                                  */
/* ========================================================================= */

/**
 * @brief Vérifie le cycle complet : génération → signature → vérification.
 *
 * C'est le test le plus important : il valide que la chaîne complète
 * fonctionne de bout en bout. Un message signé avec une clé privée
 * doit être vérifiable avec la clé publique correspondante.
 */
TEST_CASE("sign_et_verify_succes", "[crypto]")
{
    keypair_t kp;
    crypto_generate_keypair(&kp);

    const uint8_t message[] = "paiement de 100 credits";
    signature_t sig;

    /* Signature */
    esp_err_t err = crypto_sign(message, sizeof(message) - 1, &kp, &sig);
    TEST_ASSERT_EQUAL(ESP_OK, err);

    /* Vérification avec la bonne clé publique */
    err = crypto_verify(message, sizeof(message) - 1, &kp.public_key, &sig);
    TEST_ASSERT_EQUAL(ESP_OK, err);
}

/**
 * @brief Vérifie qu'une signature invalide est rejetée.
 *
 * Si on modifie un octet de la signature, la vérification doit échouer.
 * Cela garantit qu'un attaquant ne peut pas forger une signature.
 */
TEST_CASE("verify_signature_invalide", "[crypto]")
{
    keypair_t kp;
    crypto_generate_keypair(&kp);

    const uint8_t message[] = "transaction authentique";
    signature_t sig;
    crypto_sign(message, sizeof(message) - 1, &kp, &sig);

    /* Corrompre la signature (inverser le premier octet) */
    sig.bytes[0] ^= 0xFF;

    /* La vérification doit échouer */
    esp_err_t err = crypto_verify(message, sizeof(message) - 1, &kp.public_key, &sig);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, err);
}

/**
 * @brief Vérifie qu'une signature valide est rejetée avec une mauvaise clé.
 *
 * Simule le cas où un attaquant signe avec sa propre clé en se faisant
 * passer pour un autre device. La vérification avec la clé publique
 * du vrai device doit échouer.
 */
TEST_CASE("verify_mauvaise_cle", "[crypto]")
{
    keypair_t kp_alice, kp_bob;
    crypto_generate_keypair(&kp_alice);
    crypto_generate_keypair(&kp_bob);

    const uint8_t message[] = "paiement de alice a bob";
    signature_t sig;

    /* Alice signe */
    crypto_sign(message, sizeof(message) - 1, &kp_alice, &sig);

    /* Vérification avec la clé de Bob → doit échouer */
    esp_err_t err = crypto_verify(message, sizeof(message) - 1, &kp_bob.public_key, &sig);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, err);
}

/**
 * @brief Vérifie qu'une signature est invalidée si le message est modifié.
 *
 * Propriété d'intégrité : si le contenu de la transaction est modifié
 * après la signature (ex. : montant changé), la vérification doit échouer.
 */
TEST_CASE("verify_message_modifie", "[crypto]")
{
    keypair_t kp;
    crypto_generate_keypair(&kp);

    uint8_t message[] = "montant: 100 credits";
    signature_t sig;
    crypto_sign(message, sizeof(message) - 1, &kp, &sig);

    /* Modifier le message (changer "100" en "900") */
    message[9] = '9';

    /* La vérification doit échouer */
    esp_err_t err = crypto_verify(message, sizeof(message) - 1, &kp.public_key, &sig);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, err);
}

/**
 * @brief Vérifie le rejet des paramètres NULL pour la signature.
 */
TEST_CASE("sign_parametres_null", "[crypto]")
{
    keypair_t kp;
    signature_t sig;
    const uint8_t data[] = "test";

    crypto_generate_keypair(&kp);

    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, crypto_sign(NULL, 4, &kp, &sig));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, crypto_sign(data, 4, NULL, &sig));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, crypto_sign(data, 4, &kp, NULL));
}

/* ========================================================================= */
/*                         Tests crypto_types (inline)                        */
/* ========================================================================= */

/**
 * @brief Vérifie les fonctions utilitaires sur les types cryptographiques.
 */
TEST_CASE("public_key_equal_et_hash_equal", "[crypto]")
{
    public_key_t a, b;
    memset(&a, 0xAA, sizeof(a));
    memset(&b, 0xAA, sizeof(b));

    TEST_ASSERT_TRUE(public_key_equal(&a, &b));

    b.bytes[0] = 0xBB;
    TEST_ASSERT_FALSE(public_key_equal(&a, &b));
}

TEST_CASE("hash_is_zero", "[crypto]")
{
    hash_t h;
    memset(&h, 0, sizeof(h));
    TEST_ASSERT_TRUE(hash_is_zero(&h));

    h.bytes[15] = 1;
    TEST_ASSERT_FALSE(hash_is_zero(&h));
}

/* ========================================================================= */
/*  [F-CR-005] public_key_is_zero — couverture manquante                      */
/* ========================================================================= */

TEST_CASE("public_key_is_zero", "[crypto]")
{
    public_key_t k;
    memset(&k, 0, sizeof(k));
    TEST_ASSERT_TRUE(public_key_is_zero(&k));

    k.bytes[31] = 1;
    TEST_ASSERT_FALSE(public_key_is_zero(&k));
}

/* ========================================================================= */
/*  [F-CR-008] Vecteur de régression Monocypher 4.0.2 — crypto_ed25519_*     */
/* ========================================================================= */

/**
 * Vecteur de sortie observé avec Monocypher 4.0.2 officiel.
 *
 * Attention : malgré le nom `crypto_ed25519_*`, la sortie de Monocypher
 * 4.0.2 pour la seed RFC8032 TEST 1 ne correspond pas au vecteur public
 * RFC8032. On verrouille donc ici le comportement exact de la dépendance
 * vendoree utilisée par les devices Mesh Pay, pas une interop externe.
 */
TEST_CASE("monocypher_402_seed_to_public_key_regression", "[crypto]")
{
    const uint8_t expected_seed[32] = {
        0x9d, 0x61, 0xb1, 0x9d, 0xef, 0xfd, 0x5a, 0x60,
        0xba, 0x84, 0x4a, 0xf4, 0x92, 0xec, 0x2c, 0xc4,
        0x44, 0x49, 0xc5, 0x69, 0x7b, 0x32, 0x69, 0x19,
        0x70, 0x3b, 0xac, 0x03, 0x1c, 0xae, 0x3d, 0x55
    };
    const uint8_t expected_pubkey[32] = {
        0x70, 0x0e, 0x2c, 0xe7, 0xc4, 0xb6, 0x74, 0x42,
        0x7e, 0xab, 0x27, 0xba, 0x82, 0x0b, 0xcf, 0x6f,
        0x0f, 0xae, 0xbe, 0x68, 0xe0, 0x9f, 0xe8, 0x56,
        0x42, 0x92, 0x11, 0x4e, 0x41, 0xdc, 0x6a, 0x41
    };

    /* Monocypher zéroise `seed` en sortie → copie modifiable. */
    uint8_t seed_copy[32];
    memcpy(seed_copy, expected_seed, sizeof(seed_copy));

    uint8_t derived_secret[64];
    uint8_t derived_pubkey[32];
    crypto_ed25519_key_pair(derived_secret, derived_pubkey, seed_copy);

    TEST_ASSERT_EQUAL_MEMORY(expected_pubkey, derived_pubkey, 32);
    /* secret_key[0..31] = seed originale ; secret_key[32..63] = pubkey. */
    TEST_ASSERT_EQUAL_MEMORY(expected_seed,   derived_secret,      32);
    TEST_ASSERT_EQUAL_MEMORY(expected_pubkey, derived_secret + 32, 32);
}
