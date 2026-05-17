/**
 * @file test_hal_storage.c
 * @brief Tests unitaires de l'interface hal_storage via l'implémentation mock.
 *
 * Couvre les cas nominaux et les cas d'erreur pour les 6 opérations
 * de la vtable storage : u32_write/read, blob_write/read, erase, exists.
 */

#include "unity.h"
#include "hal/hal_storage.h"
#include "hal_storage_mock.h"
#include <string.h>

/* Instance mock partagée entre les tests */
static hal_storage_t s_storage;

static void hal_storage_test_reset(void)
{
    hal_storage_mock_create(&s_storage);
}

/**
 * Setup : créer un mock propre avant chaque test.
 */
__attribute__((weak)) void setUp(void)
{
    hal_storage_test_reset();
}

/**
 * Teardown : rien de spécial, le mock est statique.
 */
__attribute__((weak)) void tearDown(void)
{
    /* Pas de nettoyage nécessaire */
}

/* ================================================================
 * Tests u32_write / u32_read
 * ================================================================ */

/**
 * Test : écrire puis relire un entier 32 bits.
 */
TEST_CASE("storage_u32_write_read", "[hal_storage]")
{
    hal_storage_test_reset();

    uint32_t value = 0;

    /* Écriture */
    TEST_ASSERT_EQUAL(HAL_OK,
        s_storage.u32_write("config", "is_master", 1, s_storage.ctx));

    /* Relecture */
    TEST_ASSERT_EQUAL(HAL_OK,
        s_storage.u32_read("config", "is_master", &value, s_storage.ctx));
    TEST_ASSERT_EQUAL_UINT32(1, value);
}

/**
 * Test : lire une clé u32 qui n'existe pas retourne NOT_FOUND.
 */
TEST_CASE("storage_u32_read_not_found", "[hal_storage]")
{
    hal_storage_test_reset();

    uint32_t value = 42;

    hal_err_t err = s_storage.u32_read("config", "inexistant", &value,
                                       s_storage.ctx);
    TEST_ASSERT_EQUAL(HAL_ERR_NOT_FOUND, err);
}

/**
 * Test : écraser une valeur u32 existante.
 */
TEST_CASE("storage_u32_overwrite", "[hal_storage]")
{
    hal_storage_test_reset();

    uint32_t value = 0;

    /* Première écriture */
    s_storage.u32_write("config", "count", 10, s_storage.ctx);

    /* Écrasement */
    s_storage.u32_write("config", "count", 42, s_storage.ctx);

    /* Vérification que la nouvelle valeur est bien lue */
    s_storage.u32_read("config", "count", &value, s_storage.ctx);
    TEST_ASSERT_EQUAL_UINT32(42, value);

    /* Vérification qu'il n'y a qu'une seule entrée (pas de doublon) */
    TEST_ASSERT_EQUAL_UINT32(1, hal_storage_mock_count(&s_storage));
}

/* ================================================================
 * Tests blob_write / blob_read
 * ================================================================ */

/**
 * Test : écrire puis relire un blob de données binaires.
 */
TEST_CASE("storage_blob_write_read", "[hal_storage]")
{
    hal_storage_test_reset();

    /* Données de test : simule un keypair de 96 octets */
    uint8_t write_buf[96];
    for (int i = 0; i < 96; i++) {
        write_buf[i] = (uint8_t)(i & 0xFF);
    }

    /* Écriture */
    TEST_ASSERT_EQUAL(HAL_OK,
        s_storage.blob_write("keys", "keypair", write_buf, 96,
                             s_storage.ctx));

    /* Relecture */
    uint8_t read_buf[96] = {0};
    size_t len = sizeof(read_buf);
    TEST_ASSERT_EQUAL(HAL_OK,
        s_storage.blob_read("keys", "keypair", read_buf, &len,
                            s_storage.ctx));

    TEST_ASSERT_EQUAL(96, len);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(write_buf, read_buf, 96);
}

/**
 * Test : blob_read avec buf=NULL retourne uniquement la taille.
 */
TEST_CASE("storage_blob_read_size_only", "[hal_storage]")
{
    hal_storage_test_reset();

    uint8_t data[32];
    memset(data, 0xAB, 32);

    s_storage.blob_write("keys", "test", data, 32, s_storage.ctx);

    /* Requête taille seule */
    size_t len = 0;
    TEST_ASSERT_EQUAL(HAL_OK,
        s_storage.blob_read("keys", "test", NULL, &len, s_storage.ctx));
    TEST_ASSERT_EQUAL(32, len);
}

/**
 * Test : blob_read avec buffer trop petit retourne NO_MEM.
 */
TEST_CASE("storage_blob_read_buffer_too_small", "[hal_storage]")
{
    hal_storage_test_reset();

    uint8_t data[64];
    memset(data, 0xCD, 64);
    s_storage.blob_write("test", "big", data, 64, s_storage.ctx);

    /* Buffer trop petit (16 octets pour 64 octets de données) */
    uint8_t small_buf[16];
    size_t len = sizeof(small_buf);
    hal_err_t err = s_storage.blob_read("test", "big", small_buf, &len,
                                        s_storage.ctx);

    TEST_ASSERT_EQUAL(HAL_ERR_NO_MEM, err);
    /* len doit indiquer la taille nécessaire */
    TEST_ASSERT_EQUAL(64, len);
}

/**
 * Test : lire un blob qui n'existe pas retourne NOT_FOUND.
 */
TEST_CASE("storage_blob_read_not_found", "[hal_storage]")
{
    hal_storage_test_reset();

    uint8_t buf[32];
    size_t len = sizeof(buf);

    hal_err_t err = s_storage.blob_read("keys", "nope", buf, &len,
                                        s_storage.ctx);
    TEST_ASSERT_EQUAL(HAL_ERR_NOT_FOUND, err);
}

/* ================================================================
 * Tests erase
 * ================================================================ */

/**
 * Test : supprimer une entrée existante.
 */
TEST_CASE("storage_erase_existing", "[hal_storage]")
{
    hal_storage_test_reset();

    s_storage.u32_write("config", "flag", 1, s_storage.ctx);
    TEST_ASSERT_EQUAL_UINT32(1, hal_storage_mock_count(&s_storage));

    /* Suppression */
    TEST_ASSERT_EQUAL(HAL_OK,
        s_storage.erase("config", "flag", s_storage.ctx));
    TEST_ASSERT_EQUAL_UINT32(0, hal_storage_mock_count(&s_storage));

    /* Vérifier qu'on ne peut plus lire */
    uint32_t value;
    TEST_ASSERT_EQUAL(HAL_ERR_NOT_FOUND,
        s_storage.u32_read("config", "flag", &value, s_storage.ctx));
}

/**
 * Test : supprimer une clé inexistante retourne NOT_FOUND.
 */
TEST_CASE("storage_erase_not_found", "[hal_storage]")
{
    hal_storage_test_reset();

    hal_err_t err = s_storage.erase("config", "ghost", s_storage.ctx);
    TEST_ASSERT_EQUAL(HAL_ERR_NOT_FOUND, err);
}

/* ================================================================
 * Tests exists
 * ================================================================ */

/**
 * Test : vérifier l'existence d'une clé.
 */
TEST_CASE("storage_exists", "[hal_storage]")
{
    hal_storage_test_reset();

    bool found = false;

    /* Clé inexistante */
    s_storage.exists("config", "flag", &found, s_storage.ctx);
    TEST_ASSERT_FALSE(found);

    /* Créer la clé */
    s_storage.u32_write("config", "flag", 1, s_storage.ctx);

    /* Maintenant elle existe */
    s_storage.exists("config", "flag", &found, s_storage.ctx);
    TEST_ASSERT_TRUE(found);
}

/* ================================================================
 * Tests namespaces séparés
 * ================================================================ */

/**
 * Test : deux entrées avec la même clé dans des namespaces différents
 * sont bien distinctes.
 */
TEST_CASE("storage_separate_namespaces", "[hal_storage]")
{
    hal_storage_test_reset();

    /* Même clé "id" dans deux namespaces */
    s_storage.u32_write("config", "id", 100, s_storage.ctx);
    s_storage.u32_write("keys", "id", 200, s_storage.ctx);

    uint32_t v1 = 0, v2 = 0;
    s_storage.u32_read("config", "id", &v1, s_storage.ctx);
    s_storage.u32_read("keys", "id", &v2, s_storage.ctx);

    TEST_ASSERT_EQUAL_UINT32(100, v1);
    TEST_ASSERT_EQUAL_UINT32(200, v2);
    TEST_ASSERT_EQUAL_UINT32(2, hal_storage_mock_count(&s_storage));
}

/* ================================================================
 * Tests mock_reset et mock_count
 * ================================================================ */

/**
 * Test : reset vide toutes les entrées.
 */
TEST_CASE("storage_mock_reset", "[hal_storage]")
{
    hal_storage_test_reset();

    s_storage.u32_write("a", "x", 1, s_storage.ctx);
    s_storage.u32_write("b", "y", 2, s_storage.ctx);
    TEST_ASSERT_EQUAL_UINT32(2, hal_storage_mock_count(&s_storage));

    hal_storage_mock_reset(&s_storage);
    TEST_ASSERT_EQUAL_UINT32(0, hal_storage_mock_count(&s_storage));

    /* Vérifier que les données sont inaccessibles */
    uint32_t v;
    TEST_ASSERT_EQUAL(HAL_ERR_NOT_FOUND,
        s_storage.u32_read("a", "x", &v, s_storage.ctx));
}

/**
 * Test : paramètres NULL retournent INVALID.
 */
TEST_CASE("storage_null_params", "[hal_storage]")
{
    hal_storage_test_reset();

    TEST_ASSERT_EQUAL(HAL_ERR_INVALID,
        s_storage.u32_write(NULL, "key", 0, s_storage.ctx));
    TEST_ASSERT_EQUAL(HAL_ERR_INVALID,
        s_storage.u32_write("ns", NULL, 0, s_storage.ctx));
    TEST_ASSERT_EQUAL(HAL_ERR_INVALID,
        s_storage.u32_read("ns", "key", NULL, s_storage.ctx));
}
