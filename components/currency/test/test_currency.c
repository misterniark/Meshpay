/**
 * @file test_currency.c
 * @brief Tests unitaires du module currency (rules + melt).
 *
 * 19 tests couvrant :
 * - Rules : currency_id, expiry, transfer limits, balance+fee, supply, mint authority,
 *           cooldown, validate, null params
 * - Melt : ticks_due, BPS apply, FIXED apply, catchup, next_timestamp, disabled
 */

#include "unity.h"
#include "currency/currency_config.h"
#include "currency/currency_rules.h"
#include "currency/currency_melt.h"
#include <string.h>

/* ================================================================
 * Helpers
 * ================================================================ */

/** Créer une config de test par défaut */
static currency_config_t make_default_config(void)
{
    currency_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));

    cfg.currency_id = 0x12345678;
    strcpy(cfg.name, "TestCoin");
    strcpy(cfg.symbol, "TST");
    cfg.decimals = 2;

    /* Une seule autorité MINT */
    memset(cfg.mint_authorities[0].bytes, 0xAA, CRYPTO_PUBLIC_KEY_SIZE);
    cfg.mint_authority_count = 1;

    cfg.max_supply = 1000000;
    cfg.valid_until = 0; /* Pas d'expiration */

    cfg.min_transfer_amount = 10;
    cfg.max_transfer_amount = 50000;
    cfg.transfer_fee = 5;
    cfg.transfer_cooldown_ms = 1000; /* 1 seconde */
    cfg.initial_balance = 500;
    strcpy(cfg.description, "Festival du Soleil 2026");

    cfg.melt_enabled = false;

    return cfg;
}

/** Créer une TX TRANSFER de test */
static transaction_t make_transfer(uint32_t amount, uint32_t currency_id)
{
    transaction_t tx;
    memset(&tx, 0, sizeof(tx));
    tx.type = TX_TYPE_TRANSFER;
    tx.amount = amount;
    tx.currency_id = currency_id;
    memset(tx.from.bytes, 0x11, CRYPTO_PUBLIC_KEY_SIZE);
    memset(tx.to.bytes, 0x22, CRYPTO_PUBLIC_KEY_SIZE);
    tx.parent_count = 1;
    tx.status = TX_STATUS_LOCKED;
    return tx;
}

/** Créer une TX MINT de test */
static transaction_t make_mint(uint32_t amount, uint32_t currency_id)
{
    transaction_t tx;
    memset(&tx, 0, sizeof(tx));
    tx.type = TX_TYPE_MINT;
    tx.amount = amount;
    tx.currency_id = currency_id;
    /* from est nul pour un MINT */
    memset(tx.to.bytes, 0x33, CRYPTO_PUBLIC_KEY_SIZE);
    tx.parent_count = 1;
    tx.status = TX_STATUS_CONFIRMED;
    return tx;
}


/* ================================================================
 * Tests Rules
 * ================================================================ */

/**
 * Test 1 : currency_id correct accepté, mauvais rejeté.
 */
TEST_CASE("currency_check_id", "[currency]")
{
    currency_config_t cfg = make_default_config();

    transaction_t tx_ok = make_transfer(100, 0x12345678);
    TEST_ASSERT_EQUAL(CURRENCY_OK, currency_check_id(&cfg, &tx_ok));

    transaction_t tx_bad = make_transfer(100, 0xDEADBEEF);
    TEST_ASSERT_EQUAL(CURRENCY_ERR_WRONG_ID, currency_check_id(&cfg, &tx_bad));
}

/**
 * Test 2 : Expiration — avant = ok, après = rejeté, 0 = jamais.
 */
TEST_CASE("currency_check_expiry", "[currency]")
{
    currency_config_t cfg = make_default_config();

    /* Pas d'expiration */
    TEST_ASSERT_EQUAL(CURRENCY_OK, currency_check_expiry(&cfg, 999999999));

    /* Avec expiration */
    cfg.valid_until = 1000000;
    TEST_ASSERT_EQUAL(CURRENCY_OK, currency_check_expiry(&cfg, 999999));
    TEST_ASSERT_EQUAL(CURRENCY_OK, currency_check_expiry(&cfg, 1000000));
    TEST_ASSERT_EQUAL(CURRENCY_ERR_EXPIRED, currency_check_expiry(&cfg, 1000001));
}

/**
 * Test 3 : Limites de montant transfer.
 */
TEST_CASE("currency_check_transfer_limits", "[currency]")
{
    currency_config_t cfg = make_default_config();
    /* min = 10, max = 50000 */

    transaction_t tx_ok = make_transfer(100, cfg.currency_id);
    TEST_ASSERT_EQUAL(CURRENCY_OK, currency_check_transfer_limits(&cfg, &tx_ok));

    transaction_t tx_low = make_transfer(5, cfg.currency_id);
    TEST_ASSERT_EQUAL(CURRENCY_ERR_AMOUNT_TOO_LOW, currency_check_transfer_limits(&cfg, &tx_low));

    transaction_t tx_high = make_transfer(60000, cfg.currency_id);
    TEST_ASSERT_EQUAL(CURRENCY_ERR_AMOUNT_TOO_HIGH, currency_check_transfer_limits(&cfg, &tx_high));

    /* Les MINT ignorent les limites transfer */
    transaction_t mint = make_mint(5, cfg.currency_id);
    TEST_ASSERT_EQUAL(CURRENCY_OK, currency_check_transfer_limits(&cfg, &mint));
}

/**
 * Test 4 : Solde insuffisant avec fee.
 */
TEST_CASE("currency_check_balance_with_fee", "[currency]")
{
    currency_config_t cfg = make_default_config();

    /* Le fee est désormais dans la transaction, pas dans la config */
    transaction_t tx = make_transfer(100, cfg.currency_id);
    tx.fee = 5;

    /* Solde 105 = pile 100 + 5 → ok */
    TEST_ASSERT_EQUAL(CURRENCY_OK, currency_check_balance(&cfg, &tx, 105));

    /* Solde 104 = pas assez pour 100 + 5 → insuffisant */
    TEST_ASSERT_EQUAL(CURRENCY_ERR_INSUFFICIENT, currency_check_balance(&cfg, &tx, 104));

    /* Solde 200 → ok */
    TEST_ASSERT_EQUAL(CURRENCY_OK, currency_check_balance(&cfg, &tx, 200));
}

/**
 * Test 5 : Max supply respecté.
 */
TEST_CASE("currency_check_supply", "[currency]")
{
    currency_config_t cfg = make_default_config();
    /* max_supply = 1000000 */

    transaction_t mint = make_mint(500, cfg.currency_id);

    /* total_minted = 999500 + 500 = 1000000 → ok (pile) */
    TEST_ASSERT_EQUAL(CURRENCY_OK, currency_check_supply(&cfg, &mint, 999500));

    /* total_minted = 999501 + 500 = 1000001 → dépassé */
    TEST_ASSERT_EQUAL(CURRENCY_ERR_SUPPLY_EXCEEDED, currency_check_supply(&cfg, &mint, 999501));

    /* max_supply = 0 → illimité */
    cfg.max_supply = 0;
    TEST_ASSERT_EQUAL(CURRENCY_OK, currency_check_supply(&cfg, &mint, 99999999));
}

/**
 * Test 6 : Mint authority — clé autorisée ok, inconnue rejetée.
 */
TEST_CASE("currency_check_mint_authority", "[currency]")
{
    currency_config_t cfg = make_default_config();
    /* Autorité : clé remplie de 0xAA */

    public_key_t good_key;
    memset(good_key.bytes, 0xAA, CRYPTO_PUBLIC_KEY_SIZE);
    TEST_ASSERT_EQUAL(CURRENCY_OK, currency_check_mint_authority(&cfg, &good_key));

    public_key_t bad_key;
    memset(bad_key.bytes, 0xBB, CRYPTO_PUBLIC_KEY_SIZE);
    TEST_ASSERT_EQUAL(CURRENCY_ERR_NOT_AUTHORITY, currency_check_mint_authority(&cfg, &bad_key));
}

/**
 * Test 7 : Validation complète — TRANSFER ok.
 */
TEST_CASE("currency_validate_transfer_ok", "[currency]")
{
    currency_config_t cfg = make_default_config();

    transaction_t tx = make_transfer(100, cfg.currency_id);

    /* current_time=0 → pas de check expiry ni cooldown, sender_balance=200 */
    TEST_ASSERT_EQUAL(CURRENCY_OK, currency_validate(&cfg, &tx, 0, 200, 0, 0));
}

/**
 * Test 8 : Validation complète — premier rejet l'emporte.
 */
TEST_CASE("currency_validate_first_error_wins", "[currency]")
{
    currency_config_t cfg = make_default_config();

    /* Mauvais currency_id → rejeté avant même de vérifier le montant */
    transaction_t tx = make_transfer(5, 0xDEADBEEF);
    TEST_ASSERT_EQUAL(CURRENCY_ERR_WRONG_ID, currency_validate(&cfg, &tx, 0, 200, 0, 0));
}

/**
 * Test 9 : Paramètres NULL.
 */
TEST_CASE("currency_rules_null_params", "[currency]")
{
    currency_config_t cfg = make_default_config();
    transaction_t tx = make_transfer(100, cfg.currency_id);

    TEST_ASSERT_EQUAL(CURRENCY_ERR_NULL_PARAM, currency_check_id(NULL, &tx));
    TEST_ASSERT_EQUAL(CURRENCY_ERR_NULL_PARAM, currency_check_id(&cfg, NULL));
    TEST_ASSERT_EQUAL(CURRENCY_ERR_NULL_PARAM, currency_validate(NULL, &tx, 0, 0, 0, 0));
}

/**
 * Test 10 : Cooldown entre TRANSFER — respecté ok, trop tôt rejeté.
 */
TEST_CASE("currency_check_cooldown", "[currency]")
{
    currency_config_t cfg = make_default_config();
    /* cooldown = 1000 ms */

    transaction_t tx = make_transfer(100, cfg.currency_id);

    /* Pas de précédent → toujours ok */
    TEST_ASSERT_EQUAL(CURRENCY_OK, currency_check_cooldown(&cfg, &tx, 0, 5000));

    /* Dernier transfer à t=5000, maintenant t=5500 → trop tôt (500 < 1000) */
    TEST_ASSERT_EQUAL(CURRENCY_ERR_COOLDOWN, currency_check_cooldown(&cfg, &tx, 5000, 5500));

    /* Dernier transfer à t=5000, maintenant t=6000 → pile ok */
    TEST_ASSERT_EQUAL(CURRENCY_OK, currency_check_cooldown(&cfg, &tx, 5000, 6000));

    /* Dernier transfer à t=5000, maintenant t=7000 → largement ok */
    TEST_ASSERT_EQUAL(CURRENCY_OK, currency_check_cooldown(&cfg, &tx, 5000, 7000));

    /* Cooldown désactivé (0) → toujours ok même si très rapide */
    cfg.transfer_cooldown_ms = 0;
    TEST_ASSERT_EQUAL(CURRENCY_OK, currency_check_cooldown(&cfg, &tx, 5000, 5001));
}

/**
 * Test 11 : Cooldown intégré dans currency_validate.
 */
TEST_CASE("currency_validate_with_cooldown", "[currency]")
{
    currency_config_t cfg = make_default_config();
    /* cooldown = 1000 ms */

    transaction_t tx = make_transfer(100, cfg.currency_id);

    /* current_time=5500, last_transfer=5000 → cooldown non respecté */
    TEST_ASSERT_EQUAL(CURRENCY_ERR_COOLDOWN,
                      currency_validate(&cfg, &tx, 5500, 200, 0, 5000));

    /* current_time=6500, last_transfer=5000 → cooldown ok, tx valide */
    TEST_ASSERT_EQUAL(CURRENCY_OK,
                      currency_validate(&cfg, &tx, 6500, 200, 0, 5000));
}

/**
 * Test 12 : Description et initial_balance présents dans la config.
 */
TEST_CASE("currency_config_description_and_initial", "[currency]")
{
    currency_config_t cfg = make_default_config();

    /* Vérifier que la description est bien stockée */
    TEST_ASSERT_EQUAL_STRING("Festival du Soleil 2026", cfg.description);

    /* Vérifier le solde initial */
    TEST_ASSERT_EQUAL_UINT32(500, cfg.initial_balance);

    /* Le solde initial n'affecte pas la validation (c'est au boot) */
    transaction_t tx = make_transfer(100, cfg.currency_id);
    TEST_ASSERT_EQUAL(CURRENCY_OK, currency_validate(&cfg, &tx, 0, 200, 0, 0));
}

/* ================================================================
 * Tests Melt
 * ================================================================ */

/**
 * Test 13 : Ticks due — calcul basique.
 */
TEST_CASE("melt_ticks_due_basic", "[currency]")
{
    currency_config_t cfg = make_default_config();
    cfg.melt_enabled = true;
    cfg.melt_period_seconds = 86400; /* 1 jour */

    /* 3 jours écoulés → 3 ticks */
    uint64_t last = 0;
    uint64_t now = 3 * 86400 * 1000ULL; /* 3 jours en ms */
    TEST_ASSERT_EQUAL_UINT32(3, currency_melt_ticks_due(&cfg, last, now));

    /* Moins d'un jour → 0 ticks */
    now = 86000 * 1000ULL;
    TEST_ASSERT_EQUAL_UINT32(0, currency_melt_ticks_due(&cfg, last, now));
}

/**
 * Test 14 : Ticks due — fonte désactivée retourne 0.
 */
TEST_CASE("melt_ticks_due_disabled", "[currency]")
{
    currency_config_t cfg = make_default_config();
    cfg.melt_enabled = false;
    cfg.melt_period_seconds = 86400;

    TEST_ASSERT_EQUAL_UINT32(0, currency_melt_ticks_due(&cfg, 0, 999999999));
}

/**
 * Test 15 : Ticks due — plafond MELT_MAX_CATCHUP_TICKS.
 */
TEST_CASE("melt_ticks_due_capped", "[currency]")
{
    currency_config_t cfg = make_default_config();
    cfg.melt_enabled = true;
    cfg.melt_period_seconds = 1; /* 1 seconde */

    /* 1000 secondes écoulées mais plafond à 365 */
    uint64_t now = 1000 * 1000ULL;
    TEST_ASSERT_EQUAL_UINT32(MELT_MAX_CATCHUP_TICKS, currency_melt_ticks_due(&cfg, 0, now));
}

/**
 * Test 16 : Melt apply BPS — 1% par tick, 3 ticks.
 */
TEST_CASE("melt_apply_bps", "[currency]")
{
    currency_config_t cfg = make_default_config();
    cfg.melt_enabled = true;
    cfg.melt_volume_mode = MELT_MODE_BPS;
    cfg.melt_bps = 100; /* 1% par tick */

    /*
     * Solde initial : 10000
     * Tick 1 : 10000 * 9900/10000 = 9900
     * Tick 2 : 9900 * 9900/10000 = 9801
     * Tick 3 : 9801 * 9900/10000 = 9702 (entier tronqué : 9801*9900=97029900/10000=9702)
     */
    uint32_t result = currency_melt_apply(&cfg, 10000, 3);
    TEST_ASSERT_EQUAL_UINT32(9702, result);
}

/**
 * Test 17 : Melt apply FIXED — 50 par tick, 3 ticks.
 */
TEST_CASE("melt_apply_fixed", "[currency]")
{
    currency_config_t cfg = make_default_config();
    cfg.melt_enabled = true;
    cfg.melt_volume_mode = MELT_MODE_FIXED;
    cfg.melt_fixed_amount = 50;

    /* 10000 - 3*50 = 9850 */
    uint32_t result = currency_melt_apply(&cfg, 10000, 3);
    TEST_ASSERT_EQUAL_UINT32(9850, result);
}

/**
 * Test 18 : Melt apply — solde tombe à 0 sans underflow.
 */
TEST_CASE("melt_apply_floor_zero", "[currency]")
{
    currency_config_t cfg = make_default_config();
    cfg.melt_enabled = true;
    cfg.melt_volume_mode = MELT_MODE_FIXED;
    cfg.melt_fixed_amount = 100;

    /* 50 - 3*100 → doit être 0, pas underflow */
    uint32_t result = currency_melt_apply(&cfg, 50, 3);
    TEST_ASSERT_EQUAL_UINT32(0, result);

    /* BPS : 100% par tick (10000 bps) → solde tombe à 0 en 1 tick */
    cfg.melt_volume_mode = MELT_MODE_BPS;
    cfg.melt_bps = 10000;
    result = currency_melt_apply(&cfg, 5000, 1);
    TEST_ASSERT_EQUAL_UINT32(0, result);
}

/**
 * Test 19 : Melt next_timestamp — avance correctement.
 */
TEST_CASE("melt_next_timestamp", "[currency]")
{
    currency_config_t cfg = make_default_config();
    cfg.melt_enabled = true;
    cfg.melt_period_seconds = 86400; /* 1 jour */

    uint64_t last = 1000000; /* 1000s en ms */
    uint32_t ticks = 3;
    uint64_t now = 1000000 + 4 * 86400 * 1000ULL; /* 4 jours plus tard */

    /* Avance de 3 * 86400000 = 259200000 ms */
    uint64_t next = currency_melt_next_timestamp(&cfg, last, ticks, now);
    TEST_ASSERT_EQUAL_UINT64(1000000 + 3 * 86400 * 1000ULL, next);

    /* 0 ticks → pas de changement */
    next = currency_melt_next_timestamp(&cfg, last, 0, now);
    TEST_ASSERT_EQUAL_UINT64(last, next);
}
