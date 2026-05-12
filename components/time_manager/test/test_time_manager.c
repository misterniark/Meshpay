/**
 * @file test_time_manager.c
 * @brief Tests unitaires du gestionnaire de temps (Lamport + Master).
 *
 * 11 tests couvrant les deux modes et les cas limites :
 * - Lamport : incrément, réception TX, indépendance monotonique
 * - Master : sync, delta trop grand, multi-maître, fallback, monotonie
 */

#include "unity.h"
#include "time_manager/time_manager.h"
#include <string.h>

/* ================================================================
 * Mock du temps monotonique
 * ================================================================ */

static uint64_t s_mock_monotonic = 0;

static uint64_t mock_get_monotonic(void)
{
    return s_mock_monotonic;
}

/* Helpers pour créer des clés publiques distinctes */
static void make_key(public_key_t *key, uint8_t fill)
{
    memset(key->bytes, fill, CRYPTO_PUBLIC_KEY_SIZE);
}

/* Instance partagée */
static time_manager_t s_tm;

void setUp(void)
{
    s_mock_monotonic = 0;
    memset(&s_tm, 0, sizeof(s_tm));
}

void tearDown(void)
{
}

/* ================================================================
 * Tests Mode LAMPORT
 * ================================================================ */

/**
 * Test 1 : Incrément basique — chaque appel retourne une valeur croissante.
 */
TEST_CASE("lamport_basic_increment", "[time_manager]")
{
    time_manager_config_t cfg = {
        .mode = TIME_MODE_LAMPORT,
        .get_monotonic = mock_get_monotonic,
    };
    TEST_ASSERT_EQUAL(0, time_manager_init(&s_tm, &cfg));

    uint64_t t1 = time_manager_get_tx_timestamp(&s_tm);
    uint64_t t2 = time_manager_get_tx_timestamp(&s_tm);
    uint64_t t3 = time_manager_get_tx_timestamp(&s_tm);

    TEST_ASSERT_EQUAL_UINT64(1, t1);
    TEST_ASSERT_EQUAL_UINT64(2, t2);
    TEST_ASSERT_EQUAL_UINT64(3, t3);
}

/**
 * Test 2 : Réception d'une TX avec timestamp supérieur.
 * Le compteur doit sauter au-dessus de la valeur reçue.
 */
TEST_CASE("lamport_on_rx_higher", "[time_manager]")
{
    time_manager_config_t cfg = {
        .mode = TIME_MODE_LAMPORT,
        .get_monotonic = mock_get_monotonic,
    };
    time_manager_init(&s_tm, &cfg);

    /* Avancer le compteur à 5 */
    for (int i = 0; i < 5; i++) {
        time_manager_get_tx_timestamp(&s_tm);
    }
    TEST_ASSERT_EQUAL_UINT64(5, time_manager_get_lamport(&s_tm));

    /* Recevoir une TX avec timestamp 100 */
    time_manager_on_tx_received(&s_tm, 100);

    /* Le compteur doit être à 101 (max(5, 100) + 1) */
    TEST_ASSERT_EQUAL_UINT64(101, time_manager_get_lamport(&s_tm));

    /* Le prochain timestamp doit être 102 */
    uint64_t next = time_manager_get_tx_timestamp(&s_tm);
    TEST_ASSERT_EQUAL_UINT64(102, next);
}

/**
 * Test 3 : Réception d'une TX avec timestamp inférieur.
 * Le compteur ne doit pas reculer.
 */
TEST_CASE("lamport_on_rx_lower", "[time_manager]")
{
    time_manager_config_t cfg = {
        .mode = TIME_MODE_LAMPORT,
        .get_monotonic = mock_get_monotonic,
    };
    time_manager_init(&s_tm, &cfg);

    /* Avancer le compteur à 50 */
    s_tm.lamport_counter = 50;

    /* Recevoir une TX avec timestamp 10 */
    time_manager_on_tx_received(&s_tm, 10);

    /* Le compteur doit être à 51 (max(50, 10) + 1) */
    TEST_ASSERT_EQUAL_UINT64(51, time_manager_get_lamport(&s_tm));
}

/**
 * Test 4 : Le temps monotonique est indépendant du Lamport.
 */
TEST_CASE("lamport_monotonic_independent", "[time_manager]")
{
    time_manager_config_t cfg = {
        .mode = TIME_MODE_LAMPORT,
        .get_monotonic = mock_get_monotonic,
    };
    time_manager_init(&s_tm, &cfg);

    s_mock_monotonic = 42000;
    TEST_ASSERT_EQUAL_UINT64(42000, time_manager_get_monotonic(&s_tm));

    /* Le Lamport n'affecte pas le monotonique */
    time_manager_get_tx_timestamp(&s_tm);
    TEST_ASSERT_EQUAL_UINT64(42000, time_manager_get_monotonic(&s_tm));

    /* Le monotonique n'affecte pas le Lamport */
    s_mock_monotonic = 99000;
    TEST_ASSERT_EQUAL_UINT64(1, time_manager_get_lamport(&s_tm));
}

/* ================================================================
 * Tests Mode MASTER
 * ================================================================ */

/**
 * Test 5 : Premier sync maître accepté.
 */
TEST_CASE("master_first_sync_accepted", "[time_manager]")
{
    time_manager_config_t cfg = {
        .mode = TIME_MODE_MASTER,
        .get_monotonic = mock_get_monotonic,
    };
    time_manager_init(&s_tm, &cfg);
    TEST_ASSERT_FALSE(time_manager_has_valid_master(&s_tm));

    /* Simuler un maître avec son RTC à 1000000ms */
    s_mock_monotonic = 5000;
    public_key_t master_key;
    make_key(&master_key, 0xAA);

    int ret = time_manager_on_master_sync(&s_tm, &master_key,
                                           1000000, 50);
    TEST_ASSERT_EQUAL(0, ret);
    TEST_ASSERT_TRUE(time_manager_has_valid_master(&s_tm));

    /* Le timestamp TX doit utiliser le wall-clock corrigé */
    uint64_t ts = time_manager_get_tx_timestamp(&s_tm);
    /* wall = 5000 + (1000000 - 5000) = 1000000, mais Lamport a été incrémenté */
    TEST_ASSERT_TRUE(ts >= 1000000);
}

/**
 * Test 6 : Delta > 1h rejeté.
 */
TEST_CASE("master_delta_too_large_rejected", "[time_manager]")
{
    time_manager_config_t cfg = {
        .mode = TIME_MODE_MASTER,
        .get_monotonic = mock_get_monotonic,
    };
    time_manager_init(&s_tm, &cfg);

    /* Premier maître : accepté */
    s_mock_monotonic = 10000;
    public_key_t master_a;
    make_key(&master_a, 0xAA);
    time_manager_on_master_sync(&s_tm, &master_a, 100000, 10);

    /* Deuxième sync du même maître mais avec un delta > 1h */
    s_mock_monotonic = 11000;
    uint64_t bad_time = 100000 + TIME_MASTER_MAX_DELTA_MS + 1;
    int ret = time_manager_on_master_sync(&s_tm, &master_a, bad_time, 20);
    TEST_ASSERT_EQUAL(-1, ret);
}

/**
 * Test 7 : Multi-maître — plus petit delta gagne.
 */
TEST_CASE("master_multi_smallest_delta_wins", "[time_manager]")
{
    time_manager_config_t cfg = {
        .mode = TIME_MODE_MASTER,
        .get_monotonic = mock_get_monotonic,
    };
    time_manager_init(&s_tm, &cfg);

    s_mock_monotonic = 10000;
    public_key_t master_a, master_b;
    make_key(&master_a, 0xAA);
    make_key(&master_b, 0xBB);

    /* Maître A : offset = 100000 - 10000 = 90000 */
    time_manager_on_master_sync(&s_tm, &master_a, 100000, 10);
    TEST_ASSERT_TRUE(public_key_equal(&s_tm.current_master_key, &master_a));

    /* Maître B avec un plus grand delta par rapport au temps corrigé actuel */
    s_mock_monotonic = 10500;
    /* Temps corrigé actuel = 10500 + 90000 = 100500 */
    /* Maître B dit 105000 → delta = |105000 - 100500| = 4500 */
    /* Maître A dit (implicitement) ~100500 → delta = 0 */
    int ret = time_manager_on_master_sync(&s_tm, &master_b, 105000, 15);
    /* B a un plus grand delta que A, donc rejeté */
    TEST_ASSERT_EQUAL(-1, ret);
    TEST_ASSERT_TRUE(public_key_equal(&s_tm.current_master_key, &master_a));
}

/**
 * Test 8 : Fallback Lamport après timeout sans maître.
 */
TEST_CASE("master_fallback_on_timeout", "[time_manager]")
{
    time_manager_config_t cfg = {
        .mode = TIME_MODE_MASTER,
        .get_monotonic = mock_get_monotonic,
    };
    time_manager_init(&s_tm, &cfg);

    /* Sync maître à t=1000 */
    s_mock_monotonic = 1000;
    public_key_t master;
    make_key(&master, 0xCC);
    time_manager_on_master_sync(&s_tm, &master, 500000, 5);
    TEST_ASSERT_TRUE(time_manager_has_valid_master(&s_tm));

    /* Avancer le temps au-delà du fallback (10 min + 1ms) */
    s_mock_monotonic = 1000 + TIME_MASTER_FALLBACK_MS + 1;
    TEST_ASSERT_FALSE(time_manager_has_valid_master(&s_tm));

    /* Le timestamp doit être Lamport (petit), pas wall-clock (grand) */
    uint64_t ts = time_manager_get_tx_timestamp(&s_tm);
    /* Le Lamport est autour de 6-7 (après le sync), pas 500000+ */
    TEST_ASSERT_TRUE(ts < 1000);
}

/**
 * Test 9 : Le timestamp ne descend jamais sous le Lamport.
 */
TEST_CASE("master_never_decreasing", "[time_manager]")
{
    time_manager_config_t cfg = {
        .mode = TIME_MODE_MASTER,
        .get_monotonic = mock_get_monotonic,
    };
    time_manager_init(&s_tm, &cfg);

    /* Monter le Lamport très haut via des TX reçues */
    time_manager_on_tx_received(&s_tm, 999999);
    /* Lamport = 1000000 */

    /* Sync maître avec un temps inférieur au Lamport */
    s_mock_monotonic = 5000;
    public_key_t master;
    make_key(&master, 0xDD);
    time_manager_on_master_sync(&s_tm, &master, 50000, 100);
    /* offset = 50000 - 5000 = 45000 */
    /* wall = 5000 + 45000 = 50000 — inférieur au Lamport 1000001 */

    uint64_t ts = time_manager_get_tx_timestamp(&s_tm);
    /* Doit retourner le Lamport, pas le wall-clock */
    TEST_ASSERT_TRUE(ts > 50000);
    TEST_ASSERT_TRUE(ts >= 1000001);
}

/**
 * Test 10 : on_master_sync rejeté en mode Lamport.
 */
TEST_CASE("master_sync_rejected_in_lamport_mode", "[time_manager]")
{
    time_manager_config_t cfg = {
        .mode = TIME_MODE_LAMPORT,
        .get_monotonic = mock_get_monotonic,
    };
    time_manager_init(&s_tm, &cfg);

    public_key_t master;
    make_key(&master, 0xEE);
    int ret = time_manager_on_master_sync(&s_tm, &master, 100000, 50);
    TEST_ASSERT_EQUAL(-1, ret);
    TEST_ASSERT_FALSE(time_manager_has_valid_master(&s_tm));
}

/**
 * Test 11 : Init avec paramètres NULL retourne erreur.
 */
TEST_CASE("time_manager_init_null_params", "[time_manager]")
{
    time_manager_config_t cfg = {
        .mode = TIME_MODE_LAMPORT,
        .get_monotonic = mock_get_monotonic,
    };

    TEST_ASSERT_EQUAL(-1, time_manager_init(NULL, &cfg));
    TEST_ASSERT_EQUAL(-1, time_manager_init(&s_tm, NULL));

    cfg.get_monotonic = NULL;
    TEST_ASSERT_EQUAL(-1, time_manager_init(&s_tm, &cfg));
}
