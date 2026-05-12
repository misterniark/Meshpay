/**
 * @file test_transaction.c
 * @brief Tests unitaires pour le module core/transaction/.
 *
 * Teste la création, la sérialisation CBOR, la désérialisation
 * et la validation des transactions TRANSFER et MINT.
 */

#include "unity.h"
#include "transaction/tx_types.h"
#include "transaction/tx_create.h"
#include "transaction/tx_serialize.h"
#include "transaction/tx_validate.h"
#include "crypto/crypto_keys.h"
#include "crypto/crypto_hash.h"
#include <string.h>

/* ========================================================================= */
/*                         Helpers de test                                    */
/* ========================================================================= */

/**
 * @brief Crée un hash fictif à partir d'un octet de remplissage.
 *
 * Utile pour simuler des hashes de transactions parentes dans les tests.
 */
static void make_dummy_hash(hash_t *h, uint8_t fill)
{
    memset(h->bytes, fill, CRYPTO_HASH_SIZE);
}

/* ========================================================================= */
/*                         Tests tx_create                                    */
/* ========================================================================= */

/**
 * @brief Vérifie la création d'une transaction TRANSFER valide.
 *
 * Après création, la transaction doit avoir :
 * - un id non-nul (hash calculé)
 * - une signature non-nulle
 * - le statut LOCKED
 * - les bons champs from/to/amount
 */
TEST_CASE("create_transfer_succes", "[transaction]")
{
    keypair_t alice, bob;
    crypto_generate_keypair(&alice);
    crypto_generate_keypair(&bob);

    hash_t parent;
    make_dummy_hash(&parent, 0xAA);

    transaction_t tx;
    esp_err_t err = tx_create_transfer(&tx, &alice, &bob.public_key,
                                       100, 0, 0, 0, &parent, 1, 1000);
    TEST_ASSERT_EQUAL(ESP_OK, err);

    /* Vérifier les champs */
    TEST_ASSERT_EQUAL(TX_TYPE_TRANSFER, tx.type);
    TEST_ASSERT_EQUAL(TX_STATUS_LOCKED, tx.status);
    TEST_ASSERT_EQUAL(100, tx.amount);
    TEST_ASSERT_EQUAL(1, tx.parent_count);
    TEST_ASSERT_EQUAL(1000, tx.timestamp);
    TEST_ASSERT_TRUE(public_key_equal(&tx.from, &alice.public_key));
    TEST_ASSERT_TRUE(public_key_equal(&tx.to, &bob.public_key));

    /* Le hash (id) ne doit pas être nul */
    TEST_ASSERT_FALSE(hash_is_zero(&tx.id));
}

/**
 * @brief Vérifie la création d'une transaction MINT.
 *
 * Spécificités MINT :
 * - from contient la clé publique du device maître signataire
 * - statut initial CONFIRMED (pas besoin d'ACK)
 */
TEST_CASE("create_mint_succes", "[transaction]")
{
    keypair_t master, user;
    crypto_generate_keypair(&master);
    crypto_generate_keypair(&user);

    hash_t parent;
    make_dummy_hash(&parent, 0xBB);

    transaction_t tx;
    esp_err_t err = tx_create_mint(&tx, &master, &user.public_key,
                                   500, 0, 0, &parent, 1, 2000);
    TEST_ASSERT_EQUAL(ESP_OK, err);

    TEST_ASSERT_EQUAL(TX_TYPE_MINT, tx.type);
    TEST_ASSERT_EQUAL(TX_STATUS_CONFIRMED, tx.status);
    TEST_ASSERT_EQUAL(500, tx.amount);

    /* from contient la clé publique du maître qui a signé le MINT */
    TEST_ASSERT_TRUE(public_key_equal(&tx.from, &master.public_key));
}

/**
 * @brief Vérifie le rejet d'un montant nul.
 */
TEST_CASE("create_transfer_montant_zero", "[transaction]")
{
    keypair_t alice, bob;
    crypto_generate_keypair(&alice);
    crypto_generate_keypair(&bob);

    hash_t parent;
    make_dummy_hash(&parent, 0xCC);

    transaction_t tx;
    esp_err_t err = tx_create_transfer(&tx, &alice, &bob.public_key,
                                       0, 0, 0, 0, &parent, 1, 1000);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, err);
}

/**
 * @brief Vérifie le rejet si trop de parents.
 */
TEST_CASE("create_transfer_trop_de_parents", "[transaction]")
{
    keypair_t alice, bob;
    crypto_generate_keypair(&alice);
    crypto_generate_keypair(&bob);

    hash_t parents[3];
    for (int i = 0; i < 3; i++) make_dummy_hash(&parents[i], i + 1);

    transaction_t tx;
    esp_err_t err = tx_create_transfer(&tx, &alice, &bob.public_key,
                                       100, 0, 0, 0, parents, 3, 1000);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, err);
}

/**
 * @brief Vérifie la création avec 2 parents.
 */
TEST_CASE("create_transfer_deux_parents", "[transaction]")
{
    keypair_t alice, bob;
    crypto_generate_keypair(&alice);
    crypto_generate_keypair(&bob);

    hash_t parents[2];
    make_dummy_hash(&parents[0], 0x11);
    make_dummy_hash(&parents[1], 0x22);

    transaction_t tx;
    esp_err_t err = tx_create_transfer(&tx, &alice, &bob.public_key,
                                       50, 0, 0, 0, parents, 2, 3000);
    TEST_ASSERT_EQUAL(ESP_OK, err);
    TEST_ASSERT_EQUAL(2, tx.parent_count);
}

/* ========================================================================= */
/*                         Tests tx_serialize                                 */
/* ========================================================================= */

/**
 * @brief Vérifie le roundtrip sérialisation → désérialisation.
 *
 * C'est le test le plus critique : on crée une TX, on la sérialise
 * en CBOR, puis on la désérialise et on compare. Tous les champs
 * doivent être identiques.
 */
TEST_CASE("serialize_deserialize_roundtrip", "[transaction]")
{
    keypair_t alice, bob;
    crypto_generate_keypair(&alice);
    crypto_generate_keypair(&bob);

    hash_t parent;
    make_dummy_hash(&parent, 0xDD);

    /* Créer une transaction TRANSFER */
    transaction_t tx_original;
    tx_create_transfer(&tx_original, &alice, &bob.public_key,
                       200, 0, 0, 0, &parent, 1, 5000);

    /* Sérialiser en CBOR (full) */
    uint8_t buffer[TX_CBOR_MAX_SIZE];
    size_t len = 0;
    esp_err_t err = tx_serialize_full(&tx_original, buffer, sizeof(buffer), &len);
    TEST_ASSERT_EQUAL(ESP_OK, err);
    TEST_ASSERT_GREATER_THAN(0, len);

    /* Désérialiser */
    transaction_t tx_restored;
    err = tx_deserialize(buffer, len, &tx_restored);
    TEST_ASSERT_EQUAL(ESP_OK, err);

    /* Comparer tous les champs */
    TEST_ASSERT_TRUE(hash_equal(&tx_original.id, &tx_restored.id));
    TEST_ASSERT_EQUAL(tx_original.type, tx_restored.type);
    TEST_ASSERT_TRUE(public_key_equal(&tx_original.from, &tx_restored.from));
    TEST_ASSERT_TRUE(public_key_equal(&tx_original.to, &tx_restored.to));
    TEST_ASSERT_EQUAL(tx_original.amount, tx_restored.amount);
    TEST_ASSERT_EQUAL(tx_original.parent_count, tx_restored.parent_count);
    TEST_ASSERT_EQUAL(tx_original.timestamp, tx_restored.timestamp);
    TEST_ASSERT_EQUAL(tx_original.status, tx_restored.status);
    TEST_ASSERT_EQUAL_MEMORY(tx_original.signature.bytes,
                             tx_restored.signature.bytes,
                             CRYPTO_SIGNATURE_SIZE);
}

/**
 * @brief Vérifie que la taille CBOR reste dans la contrainte ESP-NOW (250 octets).
 *
 * Teste le pire cas : TRANSFER avec 2 parents (taille maximale).
 */
TEST_CASE("serialize_taille_max_250_octets", "[transaction]")
{
    keypair_t alice, bob;
    crypto_generate_keypair(&alice);
    crypto_generate_keypair(&bob);

    hash_t parents[2];
    make_dummy_hash(&parents[0], 0xEE);
    make_dummy_hash(&parents[1], 0xFF);

    transaction_t tx;
    tx_create_transfer(&tx, &alice, &bob.public_key, 999999, 0, 0, 0, parents, 2, UINT64_MAX);

    uint8_t buffer[300];  /* Buffer plus grand pour détecter un dépassement */
    size_t len = 0;
    esp_err_t err = tx_serialize_full(&tx, buffer, sizeof(buffer), &len);

    /* La sérialisation doit réussir et tenir dans 250 octets */
    TEST_ASSERT_EQUAL(ESP_OK, err);
    TEST_ASSERT_LESS_OR_EQUAL(TX_CBOR_MAX_SIZE, len);
}

/**
 * @brief Vérifie le roundtrip d'une transaction MINT.
 */
TEST_CASE("serialize_deserialize_mint", "[transaction]")
{
    keypair_t master, user;
    crypto_generate_keypair(&master);
    crypto_generate_keypair(&user);

    hash_t parent;
    make_dummy_hash(&parent, 0x99);

    transaction_t tx_original;
    tx_create_mint(&tx_original, &master, &user.public_key, 1000, 0, 0, &parent, 1, 7000);

    uint8_t buffer[TX_CBOR_MAX_SIZE];
    size_t len = 0;
    tx_serialize_full(&tx_original, buffer, sizeof(buffer), &len);

    transaction_t tx_restored;
    esp_err_t err = tx_deserialize(buffer, len, &tx_restored);
    TEST_ASSERT_EQUAL(ESP_OK, err);

    TEST_ASSERT_EQUAL(TX_TYPE_MINT, tx_restored.type);
    TEST_ASSERT_EQUAL(TX_STATUS_CONFIRMED, tx_restored.status);
    TEST_ASSERT_EQUAL(1000, tx_restored.amount);
}

/* ========================================================================= */
/*                         Tests tx_validate                                  */
/* ========================================================================= */

/**
 * @brief Vérifie la validation structurelle d'une transaction correcte.
 */
TEST_CASE("validate_structure_succes", "[transaction]")
{
    keypair_t alice, bob;
    crypto_generate_keypair(&alice);
    crypto_generate_keypair(&bob);

    hash_t parent;
    make_dummy_hash(&parent, 0xAA);

    transaction_t tx;
    tx_create_transfer(&tx, &alice, &bob.public_key, 100, 0, 0, 0, &parent, 1, 1000);

    TEST_ASSERT_EQUAL(ESP_OK, tx_validate_structure(&tx));
}

/**
 * @brief Vérifie le rejet d'un TRANSFER avec from nul.
 */
TEST_CASE("validate_structure_transfer_from_nul", "[transaction]")
{
    keypair_t alice, bob;
    crypto_generate_keypair(&alice);
    crypto_generate_keypair(&bob);

    hash_t parent;
    make_dummy_hash(&parent, 0xAA);

    transaction_t tx;
    tx_create_transfer(&tx, &alice, &bob.public_key, 100, 0, 0, 0, &parent, 1, 1000);

    /* Corrompre le champ from */
    memset(&tx.from, 0, sizeof(public_key_t));

    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, tx_validate_structure(&tx));
}

/**
 * @brief Vérifie la validation de la signature d'un TRANSFER.
 *
 * Une transaction créée correctement doit passer la validation
 * de signature sans problème.
 */
TEST_CASE("validate_signature_transfer_succes", "[transaction]")
{
    keypair_t alice, bob;
    crypto_generate_keypair(&alice);
    crypto_generate_keypair(&bob);

    hash_t parent;
    make_dummy_hash(&parent, 0xAA);

    transaction_t tx;
    tx_create_transfer(&tx, &alice, &bob.public_key, 100, 0, 0, 0, &parent, 1, 1000);

    TEST_ASSERT_EQUAL(ESP_OK, tx_validate_signature(&tx));
}

/**
 * @brief Vérifie la validation de la signature d'un MINT.
 *
 * Depuis la correction C2, la signature des MINT est également vérifiée
 * par tx_validate_signature (elle n'est plus sautée).
 */
TEST_CASE("validate_signature_mint_succes", "[transaction]")
{
    keypair_t master, user;
    crypto_generate_keypair(&master);
    crypto_generate_keypair(&user);

    hash_t parent;
    make_dummy_hash(&parent, 0xBB);

    transaction_t tx;
    tx_create_mint(&tx, &master, &user.public_key, 500, 0, 0, &parent, 1, 2000);

    /* La signature du MINT doit être valide (vérifiée avec tx->from = clé maître) */
    TEST_ASSERT_EQUAL(ESP_OK, tx_validate_signature(&tx));
}

/**
 * @brief Vérifie qu'une transaction avec un montant modifié est rejetée.
 *
 * Simule une attaque où quelqu'un modifie le montant d'une transaction
 * après sa signature. Le hash ne correspondra plus et la validation
 * doit échouer.
 */
TEST_CASE("validate_signature_montant_modifie", "[transaction]")
{
    keypair_t alice, bob;
    crypto_generate_keypair(&alice);
    crypto_generate_keypair(&bob);

    hash_t parent;
    make_dummy_hash(&parent, 0xAA);

    transaction_t tx;
    tx_create_transfer(&tx, &alice, &bob.public_key, 100, 0, 0, 0, &parent, 1, 1000);

    /* Modifier le montant après signature */
    tx.amount = 999;

    /* La validation doit échouer (hash ne correspond plus) */
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, tx_validate_signature(&tx));
}

/**
 * @brief Vérifie la validation d'un MINT avec une clé maître autorisée.
 */
TEST_CASE("validate_master_autorise", "[transaction]")
{
    keypair_t master, user;
    crypto_generate_keypair(&master);
    crypto_generate_keypair(&user);

    hash_t parent;
    make_dummy_hash(&parent, 0xBB);

    transaction_t tx;
    tx_create_mint(&tx, &master, &user.public_key, 500, 0, 0, &parent, 1, 2000);

    /* La clé maître est dans la liste des clés autorisées */
    master_keys_t mk = {
        .keys = &master.public_key,
        .count = 1
    };

    TEST_ASSERT_EQUAL(ESP_OK, tx_validate_master(&tx, &mk));
}

/**
 * @brief Vérifie le rejet d'un MINT avec une clé non autorisée.
 */
TEST_CASE("validate_master_non_autorise", "[transaction]")
{
    keypair_t fake_master, real_master, user;
    crypto_generate_keypair(&fake_master);
    crypto_generate_keypair(&real_master);
    crypto_generate_keypair(&user);

    hash_t parent;
    make_dummy_hash(&parent, 0xCC);

    /* MINT signé par fake_master */
    transaction_t tx;
    tx_create_mint(&tx, &fake_master, &user.public_key, 500, 0, 0, &parent, 1, 2000);

    /* Seul real_master est autorisé */
    master_keys_t mk = {
        .keys = &real_master.public_key,
        .count = 1
    };

    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, tx_validate_master(&tx, &mk));
}

/**
 * @brief Vérifie que tx_validate_master retourne OK pour un TRANSFER.
 *
 * Les TRANSFER ne nécessitent pas de vérification maître.
 */
TEST_CASE("validate_master_transfer_skip", "[transaction]")
{
    keypair_t alice, bob;
    crypto_generate_keypair(&alice);
    crypto_generate_keypair(&bob);

    hash_t parent;
    make_dummy_hash(&parent, 0xDD);

    transaction_t tx;
    tx_create_transfer(&tx, &alice, &bob.public_key, 100, 0, 0, 0, &parent, 1, 1000);

    master_keys_t mk = {
        .keys = &alice.public_key,
        .count = 1
    };

    /* Doit retourner OK sans vérification */
    TEST_ASSERT_EQUAL(ESP_OK, tx_validate_master(&tx, &mk));
}
