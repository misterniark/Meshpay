/**
 * @file test_nonce_cache.c
 * @brief Tests unitaires du cache circulaire de nonces anti-rejeu.
 *
 * Mise a jour audit Lot B : le cache utilise un compteur `filled`
 * qui ferme le faux-positif sur nonce=0. La taille effective du
 * cache est definie par NONCE_CACHE_SIZE dans nonce_cache.h (les
 * tests ci-dessous referencent toujours la constante, jamais une
 * valeur littérale, pour rester cohérents avec d'éventuels
 * redimensionnements futurs). [F-EN-007]
 *
 * Utilise le framework Unity (ESP-IDF). Chaque test travaille sur
 * une instance locale s_cache, reinitialisee par setUp() avant
 * chaque test.
 */

#include "unity.h"
#include "comm/nonce_cache.h"

/* ================================================================
 * Fixture : cache reinitialise avant chaque test
 * ================================================================ */

static nonce_cache_t s_cache;

static void nonce_cache_test_reset(void)
{
    nonce_cache_init(&s_cache);
}

__attribute__((weak)) void setUp(void)
{
    nonce_cache_test_reset();
}

__attribute__((weak)) void tearDown(void)
{
    /* Rien a liberer */
}

/* ================================================================
 * Tests
 * ================================================================ */

/**
 * Apres initialisation, aucune valeur ne doit etre consideree comme
 * deja vue — y compris le nonce 0 (correction Lot B du faux-positif).
 */
TEST_CASE("nonce_cache_init_vide", "[nonce_cache]")
{
    nonce_cache_test_reset();

    /* Apres init, aucun nonce n'a ete vu, quelle que soit la valeur */
    TEST_ASSERT_FALSE(nonce_cache_seen(&s_cache, 0));
    TEST_ASSERT_FALSE(nonce_cache_seen(&s_cache, 1));
    TEST_ASSERT_FALSE(nonce_cache_seen(&s_cache, 42));
    TEST_ASSERT_FALSE(nonce_cache_seen(&s_cache, 0xFFFFFFFF));
    TEST_ASSERT_FALSE(nonce_cache_seen(&s_cache, 0xDEADBEEF));

    /* Etat interne attendu */
    TEST_ASSERT_EQUAL_UINT16(0, s_cache.idx);
    TEST_ASSERT_EQUAL_UINT16(0, s_cache.filled);
}

/**
 * Ajout d'un nonce unique puis verification de sa presence et de
 * l'absence d'un autre nonce.
 */
TEST_CASE("nonce_cache_add_et_seen", "[nonce_cache]")
{
    nonce_cache_test_reset();

    nonce_cache_add(&s_cache, 42);

    TEST_ASSERT_TRUE(nonce_cache_seen(&s_cache, 42));
    TEST_ASSERT_FALSE(nonce_cache_seen(&s_cache, 43));
    TEST_ASSERT_EQUAL_UINT16(1, s_cache.filled);
}

/**
 * Le nonce 0 est traite comme n'importe quelle autre valeur :
 * absent apres init, present apres add explicite.
 *
 * Ce test couvre la correction du bug [Lot B item 2] : avant, le 0
 * etait toujours considere comme deja vu apres init a cause du memset.
 */
TEST_CASE("nonce_cache_zero_traite_comme_normal", "[nonce_cache]")
{
    nonce_cache_test_reset();

    /* Apres init : 0 n'a pas ete vu */
    TEST_ASSERT_FALSE(nonce_cache_seen(&s_cache, 0));

    /* Apres ajout : 0 est vu */
    nonce_cache_add(&s_cache, 0);
    TEST_ASSERT_TRUE(nonce_cache_seen(&s_cache, 0));
    TEST_ASSERT_EQUAL_UINT16(1, s_cache.filled);

    /* Autres valeurs toujours absentes */
    TEST_ASSERT_FALSE(nonce_cache_seen(&s_cache, 1));
    TEST_ASSERT_FALSE(nonce_cache_seen(&s_cache, 42));
}

/**
 * Ajout de plusieurs nonces distincts et verification que chacun
 * est retrouve dans le cache.
 */
TEST_CASE("nonce_cache_multiple", "[nonce_cache]")
{
    nonce_cache_test_reset();

    const uint32_t nonces[] = {10, 20, 30, 40, 50};
    const int count = sizeof(nonces) / sizeof(nonces[0]);

    for (int i = 0; i < count; i++) {
        nonce_cache_add(&s_cache, nonces[i]);
    }

    /* Tous les nonces ajoutes doivent etre presents */
    for (int i = 0; i < count; i++) {
        TEST_ASSERT_TRUE(nonce_cache_seen(&s_cache, nonces[i]));
    }

    /* Un nonce non ajoute ne doit pas etre present */
    TEST_ASSERT_FALSE(nonce_cache_seen(&s_cache, 99));
    TEST_ASSERT_EQUAL_UINT16(count, s_cache.filled);
}

/**
 * Le compteur `filled` doit saturer a NONCE_CACHE_SIZE et ne pas
 * deborder, meme apres plusieurs tours complets du buffer circulaire.
 */
TEST_CASE("nonce_cache_filled_sature", "[nonce_cache]")
{
    nonce_cache_test_reset();

    /* Remplir exactement le cache */
    for (uint32_t i = 1; i <= NONCE_CACHE_SIZE; i++) {
        nonce_cache_add(&s_cache, i);
    }
    TEST_ASSERT_EQUAL_UINT16(NONCE_CACHE_SIZE, s_cache.filled);
    TEST_ASSERT_EQUAL_UINT16(0, s_cache.idx); /* tour complet : idx revient a 0 */

    /* Continuer a ajouter : filled doit rester a SIZE */
    for (uint32_t i = NONCE_CACHE_SIZE + 1; i <= NONCE_CACHE_SIZE * 3; i++) {
        nonce_cache_add(&s_cache, i);
    }
    TEST_ASSERT_EQUAL_UINT16(NONCE_CACHE_SIZE, s_cache.filled);
}

/**
 * Wrap-around : apres avoir rempli exactement NONCE_CACHE_SIZE entrees,
 * l'ajout d'une nouvelle entree doit evincer la plus ancienne (FIFO).
 */
TEST_CASE("nonce_cache_wrap_around", "[nonce_cache]")
{
    nonce_cache_test_reset();

    /* Remplir le cache avec les nonces 1..SIZE */
    for (uint32_t i = 1; i <= NONCE_CACHE_SIZE; i++) {
        nonce_cache_add(&s_cache, i);
    }

    /* Tous les nonces 1..SIZE doivent etre presents */
    for (uint32_t i = 1; i <= NONCE_CACHE_SIZE; i++) {
        TEST_ASSERT_TRUE(nonce_cache_seen(&s_cache, i));
    }

    /* Ajouter un nonce supplementaire : il evince l'entree a l'idx 0 (nonce 1) */
    nonce_cache_add(&s_cache, NONCE_CACHE_SIZE + 1);

    /* Le nonce 1 (le plus ancien) doit avoir ete evince */
    TEST_ASSERT_FALSE(nonce_cache_seen(&s_cache, 1));

    /* Le nouveau nonce doit etre present */
    TEST_ASSERT_TRUE(nonce_cache_seen(&s_cache, NONCE_CACHE_SIZE + 1));

    /* Les nonces 2..SIZE doivent toujours etre presents */
    for (uint32_t i = 2; i <= NONCE_CACHE_SIZE; i++) {
        TEST_ASSERT_TRUE(nonce_cache_seen(&s_cache, i));
    }
}

/**
 * Ajout de 2x la taille du cache. Seuls les SIZE derniers
 * doivent etre presents dans le cache.
 */
TEST_CASE("nonce_cache_wrap_complet", "[nonce_cache]")
{
    nonce_cache_test_reset();

    const uint32_t total = NONCE_CACHE_SIZE * 2;

    /* Ajout des nonces 1..2*SIZE */
    for (uint32_t i = 1; i <= total; i++) {
        nonce_cache_add(&s_cache, i);
    }

    /* Les nonces 1..SIZE (premiere moitie) doivent avoir ete evinces */
    for (uint32_t i = 1; i <= NONCE_CACHE_SIZE; i++) {
        TEST_ASSERT_FALSE(nonce_cache_seen(&s_cache, i));
    }

    /* Les nonces SIZE+1..2*SIZE (deuxieme moitie) doivent etre presents */
    for (uint32_t i = NONCE_CACHE_SIZE + 1; i <= total; i++) {
        TEST_ASSERT_TRUE(nonce_cache_seen(&s_cache, i));
    }
}

/**
 * Ajout du meme nonce deux fois de suite. Le cache ne doit pas
 * se corrompre et le nonce doit rester visible. filled = 2 (deux
 * entrees occupees, meme si valeurs identiques).
 */
TEST_CASE("nonce_cache_doublon", "[nonce_cache]")
{
    nonce_cache_test_reset();

    nonce_cache_add(&s_cache, 42);
    nonce_cache_add(&s_cache, 42);

    /* Le nonce doit etre present (il apparait 2 fois dans le buffer) */
    TEST_ASSERT_TRUE(nonce_cache_seen(&s_cache, 42));

    /* L'index doit avoir avance de 2, filled aussi */
    TEST_ASSERT_EQUAL_UINT16(2, s_cache.idx);
    TEST_ASSERT_EQUAL_UINT16(2, s_cache.filled);

    /* On peut encore ajouter d'autres nonces normalement */
    nonce_cache_add(&s_cache, 100);
    TEST_ASSERT_TRUE(nonce_cache_seen(&s_cache, 100));
    TEST_ASSERT_TRUE(nonce_cache_seen(&s_cache, 42));
}
