/**
 * @file test_lora_sync_jitter.c
 * @brief Tests unitaires des helpers PURS de jitter du cycle LoRa.
 *
 * Les fonctions testées sont déterministes (la source d'aléa est un
 * paramètre), donc on peut couvrir tous les cas limites sans mock.
 */

#include "unity.h"
#include "lora_sync_jitter.h"  /* header privé du composant lora_sync */
#include <stdint.h>

/* ================================================================
 * lora_jitter_initial_ms : fenêtre [0, base_ms]
 * ================================================================ */

/**
 * Cas base nul : doit toujours retourner 0 quelle que soit l'entropie.
 */
TEST_CASE("jitter_initial_base_nul", "[lora_jitter]")
{
    TEST_ASSERT_EQUAL_UINT32(0, lora_jitter_initial_ms(0, 0));
    TEST_ASSERT_EQUAL_UINT32(0, lora_jitter_initial_ms(0, 0xFFFFFFFFu));
    TEST_ASSERT_EQUAL_UINT32(0, lora_jitter_initial_ms(0, 12345));
}

/**
 * Cas rnd=0 : doit retourner 0 (borne basse).
 */
TEST_CASE("jitter_initial_rnd_zero_donne_borne_basse", "[lora_jitter]")
{
    TEST_ASSERT_EQUAL_UINT32(0, lora_jitter_initial_ms(120000, 0));
}

/**
 * Cas rnd = base_ms : doit retourner exactement base_ms (borne haute
 * incluse, car le modulo est base_ms+1).
 */
TEST_CASE("jitter_initial_borne_haute_incluse", "[lora_jitter]")
{
    /* rnd = 120000 → 120000 % 120001 = 120000 */
    TEST_ASSERT_EQUAL_UINT32(120000, lora_jitter_initial_ms(120000, 120000));
}

/**
 * Résultat toujours dans [0, base_ms] pour 1000 entropies arbitraires.
 */
TEST_CASE("jitter_initial_toujours_dans_fenetre", "[lora_jitter]")
{
    const uint32_t base = 120000;
    for (uint32_t i = 0; i < 1000; i++) {
        /* PRNG simple (xorshift 32) pour balayer l'espace d'entropie. */
        uint32_t rnd = i * 2654435761u + 0xDEADBEEFu;
        uint32_t d = lora_jitter_initial_ms(base, rnd);
        TEST_ASSERT_TRUE_MESSAGE(d <= base,
            "jitter initial hors fenetre superieure");
    }
}

/* ================================================================
 * lora_jitter_around_ms : fenêtre [base - delta, base + delta]
 * ================================================================ */

/**
 * Cas base nul : retourne 0 (intervalle degenere).
 */
TEST_CASE("jitter_around_base_nul", "[lora_jitter]")
{
    TEST_ASSERT_EQUAL_UINT32(0, lora_jitter_around_ms(0, 25, 0xABCDEF));
}

/**
 * pct = 0 : pas de jitter → delta=0 → retourne base_ms.
 */
TEST_CASE("jitter_around_pct_zero_pas_de_variation", "[lora_jitter]")
{
    TEST_ASSERT_EQUAL_UINT32(120000,
        lora_jitter_around_ms(120000, 0, 0xFFFFFFFFu));
    TEST_ASSERT_EQUAL_UINT32(120000,
        lora_jitter_around_ms(120000, 0, 0));
}

/**
 * pct = 25, rnd = 0 : doit retourner la borne basse (base - delta).
 *   delta = 120000 * 25 / 100 = 30000
 *   borne basse = 120000 - 30000 = 90000
 */
TEST_CASE("jitter_around_rnd_zero_donne_borne_basse", "[lora_jitter]")
{
    TEST_ASSERT_EQUAL_UINT32(90000, lora_jitter_around_ms(120000, 25, 0));
}

/**
 * pct = 25, rnd = 2*delta : doit retourner la borne haute (base + delta).
 *   delta=30000 → window=60001 → rnd=60000 → offset=60000 → 90000+60000=150000
 */
TEST_CASE("jitter_around_borne_haute_atteignable", "[lora_jitter]")
{
    /* On choisit rnd tel que rnd % 60001 = 60000. */
    uint32_t rnd = 60000;
    TEST_ASSERT_EQUAL_UINT32(150000, lora_jitter_around_ms(120000, 25, rnd));
}

/**
 * pct hors plage : 100 et 200 doivent être saturés à 99 (jamais 100+).
 *   Avec pct=99, delta = 120000*99/100 = 118800, borne basse = 1200 > 0.
 */
TEST_CASE("jitter_around_pct_sature_a_99", "[lora_jitter]")
{
    /* rnd=0 → borne basse. Pour pct=99 → base-delta = 120000 - 118800 = 1200. */
    TEST_ASSERT_EQUAL_UINT32(1200, lora_jitter_around_ms(120000, 100, 0));
    TEST_ASSERT_EQUAL_UINT32(1200, lora_jitter_around_ms(120000, 200, 0));
    TEST_ASSERT_EQUAL_UINT32(1200, lora_jitter_around_ms(120000, 99, 0));
}

/**
 * Résultat toujours dans la fenêtre attendue sur 1000 entropies.
 */
TEST_CASE("jitter_around_toujours_dans_fenetre", "[lora_jitter]")
{
    const uint32_t base = 120000;
    const uint32_t pct  = 25;
    const uint32_t delta = base * pct / 100;  /* 30000 */
    const uint32_t lo = base - delta;          /* 90000  */
    const uint32_t hi = base + delta;          /* 150000 */
    for (uint32_t i = 0; i < 1000; i++) {
        uint32_t rnd = i * 2654435761u + 0xC0FFEEu;
        uint32_t d = lora_jitter_around_ms(base, pct, rnd);
        TEST_ASSERT_TRUE_MESSAGE(d >= lo, "jitter around < borne basse");
        TEST_ASSERT_TRUE_MESSAGE(d <= hi, "jitter around > borne haute");
    }
}

/**
 * Petit base_ms avec petit pct → delta arrondi à 0 → retourne base.
 *   base=100, pct=0..0 -> delta=0
 *   En revanche base=100 pct=1 -> delta = 1 -> jitter [99..101]
 */
TEST_CASE("jitter_around_petit_base_clamp_delta", "[lora_jitter]")
{
    /* base=10, pct=5 → delta = 0 (10*5/100=0) → return base */
    TEST_ASSERT_EQUAL_UINT32(10, lora_jitter_around_ms(10, 5, 12345));
    /* base=100, pct=1 → delta = 1 → window=3 → offset 0..2 → 99/100/101 */
    uint32_t v0 = lora_jitter_around_ms(100, 1, 0);  /* offset 0 */
    uint32_t v1 = lora_jitter_around_ms(100, 1, 1);  /* offset 1 */
    uint32_t v2 = lora_jitter_around_ms(100, 1, 2);  /* offset 2 */
    TEST_ASSERT_EQUAL_UINT32(99,  v0);
    TEST_ASSERT_EQUAL_UINT32(100, v1);
    TEST_ASSERT_EQUAL_UINT32(101, v2);
}
