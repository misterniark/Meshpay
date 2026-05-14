/**
 * @file test_power_manager.c
 * @brief Tests de la machine d'energie power_manager.
 *
 * power_manager.c etant dependance-pure, tous les tests injectent des
 * fausses dependances : horloge simulee, source d'alim forcee, et des
 * mouchards qui enregistrent les appels backlight / esp_pm.
 */

#include "unity.h"
#include "power_manager.h"

/* --- Fausses dependances injectees --- */

static uint64_t           s_fake_now_ms;
static hal_power_source_t s_fake_source;
static uint8_t            s_last_backlight;
static int                s_backlight_calls;
static power_state_t      s_last_pm_state;
static int                s_pm_calls;

static uint64_t           fake_get_time_ms(void)        { return s_fake_now_ms; }
static hal_power_source_t fake_get_power_source(void)   { return s_fake_source; }
static void               fake_set_backlight(uint8_t p) { s_last_backlight = p; s_backlight_calls++; }
static void               fake_apply_pm(power_state_t s){ s_last_pm_state = s; s_pm_calls++; }
static void               fake_lock(void)               { /* mono-thread : no-op */ }
static void               fake_unlock(void)             { /* mono-thread : no-op */ }

/* Reinitialise les mouchards et initialise power_manager avec une
 * source d'alim donnee et un timeout court (5 s) pour des tests rapides. */
static void setup(hal_power_source_t source)
{
    s_fake_now_ms     = 1000;   /* depart non nul */
    s_fake_source     = source;
    s_last_backlight  = 255;
    s_backlight_calls = 0;
    s_last_pm_state   = (power_state_t)-1;
    s_pm_calls        = 0;

    power_manager_config_t cfg = {
        .get_time_ms      = fake_get_time_ms,
        .get_power_source = fake_get_power_source,
        .set_backlight    = fake_set_backlight,
        .apply_pm_config  = fake_apply_pm,
        .lock             = fake_lock,
        .unlock           = fake_unlock,
        .eco_timeout_ms   = 5000u,   /* 5 s pour les tests */
    };
    power_manager_init(&cfg);
}

TEST_CASE("power_usb_never_enters_eco", "[power_manager]")
{
    setup(POWER_SOURCE_USB);

    /* Etat initial attendu : ACTIF. */
    TEST_ASSERT_EQUAL(POWER_STATE_ACTIF, power_manager_get_state());

    /* 10 minutes simulees sans aucune activite. */
    for (int i = 0; i < 600; i++) {
        s_fake_now_ms += 1000;
        power_manager_tick();
    }

    TEST_ASSERT_EQUAL(POWER_STATE_ACTIF, power_manager_get_state());
    /* Aucun passage en ECO => aucun appel backlight/pm declenche par tick. */
    TEST_ASSERT_EQUAL(0, s_backlight_calls);
    TEST_ASSERT_EQUAL(0, s_pm_calls);
}

TEST_CASE("power_battery_enters_eco_after_timeout", "[power_manager]")
{
    setup(POWER_SOURCE_BATTERY);

    /* 4 s d'inactivite : pas encore le timeout (5 s). */
    s_fake_now_ms += 4000;
    power_manager_tick();
    TEST_ASSERT_EQUAL(POWER_STATE_ACTIF, power_manager_get_state());

    /* 2 s de plus => 6 s >= 5 s => passage en ECO. */
    s_fake_now_ms += 2000;
    power_manager_tick();
    TEST_ASSERT_EQUAL(POWER_STATE_ECO, power_manager_get_state());

    /* Effets de bord du passage en ECO. */
    TEST_ASSERT_EQUAL(0, s_last_backlight);
    TEST_ASSERT_EQUAL(POWER_STATE_ECO, s_last_pm_state);
    TEST_ASSERT_EQUAL(1, s_backlight_calls);
    TEST_ASSERT_EQUAL(1, s_pm_calls);

    /* Idempotence : un tick supplementaire ne re-declenche rien. */
    s_fake_now_ms += 1000;
    power_manager_tick();
    TEST_ASSERT_EQUAL(1, s_backlight_calls);
    TEST_ASSERT_EQUAL(1, s_pm_calls);
}

TEST_CASE("power_activity_returns_to_actif", "[power_manager]")
{
    setup(POWER_SOURCE_BATTERY);

    /* Forcer le passage en ECO. */
    s_fake_now_ms += 6000;
    power_manager_tick();
    TEST_ASSERT_EQUAL(POWER_STATE_ECO, power_manager_get_state());

    /* Une interaction => retour immediat en ACTIF. */
    s_fake_now_ms += 500;
    power_manager_notify_activity();

    TEST_ASSERT_EQUAL(POWER_STATE_ACTIF, power_manager_get_state());
    TEST_ASSERT_EQUAL(100, s_last_backlight);
    TEST_ASSERT_EQUAL(POWER_STATE_ACTIF, s_last_pm_state);
    /* 2 appels backlight au total : 1 pour ECO, 1 pour le retour ACTIF. */
    TEST_ASSERT_EQUAL(2, s_backlight_calls);
    TEST_ASSERT_EQUAL(2, s_pm_calls);
}
