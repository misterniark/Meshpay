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
