/**
 * @file test_ui_pin.c
 * @brief Tests unitaires pour le module ui/ui_pin.
 *
 * Teste la détection de PIN faibles, l'enregistrement et la vérification
 * de PIN via PBKDF2-HMAC-SHA256, ainsi que la logique anti brute-force
 * (compteur d'échecs, blocage après 10 tentatives).
 *
 * Le stockage NVS est simulé par hal_storage_mock (en mémoire RAM).
 */

#include "unity.h"
#include "ui/ui_pin.h"
#include "crypto/crypto_types.h"
#include "hal_storage_mock.h"
#include <string.h>

/* ========================================================================= */
/*                         Fixtures                                           */
/* ========================================================================= */

/** Instance mock du stockage, réinitialisée entre chaque test */
static hal_storage_t s_storage;

static void ui_pin_test_reset(void)
{
    hal_storage_mock_create(&s_storage);
}

__attribute__((weak)) void setUp(void)
{
    ui_pin_test_reset();
}

__attribute__((weak)) void tearDown(void)
{
    hal_storage_mock_reset(&s_storage);
}

/* ========================================================================= */
/*                         Tests de détection de PIN faibles                  */
/* ========================================================================= */

/**
 * @brief Vérifie que tous les PIN faibles connus sont détectés.
 *
 * Scénario : on teste les 10 répétitions (0000-9999), les séquences
 * ascendantes/descendantes, les années courantes, et les motifs.
 * Résultat attendu : ui_pin_is_weak retourne true pour chacun.
 * On vérifie aussi qu'un PIN valide comme {7,3,9,1} retourne false.
 */
TEST_CASE("pin_weak_detection", "[ui_pin]")
{
    ui_pin_test_reset();

    /* Répétitions simples (0000 à 9999) */
    for (uint8_t d = 0; d <= 9; d++) {
        const uint8_t pin[UI_PIN_LENGTH] = {d, d, d, d};
        TEST_ASSERT_TRUE_MESSAGE(ui_pin_is_weak(pin),
            "Les répétitions doivent être faibles");
    }

    /* Séquences ascendantes et descendantes */
    const uint8_t seq_asc[] = {1, 2, 3, 4};
    TEST_ASSERT_TRUE(ui_pin_is_weak(seq_asc));

    const uint8_t seq_desc[] = {4, 3, 2, 1};
    TEST_ASSERT_TRUE(ui_pin_is_weak(seq_desc));

    const uint8_t seq_0123[] = {0, 1, 2, 3};
    TEST_ASSERT_TRUE(ui_pin_is_weak(seq_0123));

    /* Années courantes */
    const uint8_t year_2024[] = {2, 0, 2, 4};
    TEST_ASSERT_TRUE(ui_pin_is_weak(year_2024));

    /* Motifs courants */
    const uint8_t pattern_1357[] = {1, 3, 5, 7};
    TEST_ASSERT_TRUE(ui_pin_is_weak(pattern_1357));

    const uint8_t pattern_2468[] = {2, 4, 6, 8};
    TEST_ASSERT_TRUE(ui_pin_is_weak(pattern_2468));

    /* Un PIN valide ne doit PAS être détecté comme faible */
    const uint8_t valid_pin[] = {7, 3, 9, 1};
    TEST_ASSERT_FALSE_MESSAGE(ui_pin_is_weak(valid_pin),
        "Un PIN non trivial ne doit pas être considéré comme faible");
}

/* ========================================================================= */
/*                         Tests d'enregistrement                             */
/* ========================================================================= */

/**
 * @brief Enregistrement d'un PIN valide.
 *
 * Scénario : on enregistre le PIN {7,3,9,1}.
 * Résultat attendu : ui_pin_register retourne UI_PIN_OK
 * et ui_pin_is_configured retourne true.
 */
TEST_CASE("pin_register_ok", "[ui_pin]")
{
    ui_pin_test_reset();

    const uint8_t pin[] = {7, 3, 9, 1};

    ui_pin_result_t result = ui_pin_register(pin, &s_storage);

    TEST_ASSERT_EQUAL(UI_PIN_OK, result);
    TEST_ASSERT_TRUE(ui_pin_is_configured(&s_storage));
}

/**
 * @brief Rejet d'un PIN faible lors de l'enregistrement.
 *
 * Scénario : on tente d'enregistrer {1,2,3,4} (séquence triviale).
 * Résultat attendu : ui_pin_register retourne UI_PIN_WEAK
 * et aucun PIN ne doit être stocké.
 */
TEST_CASE("pin_register_weak_rejected", "[ui_pin]")
{
    ui_pin_test_reset();

    const uint8_t weak_pin[] = {1, 2, 3, 4};

    ui_pin_result_t result = ui_pin_register(weak_pin, &s_storage);

    TEST_ASSERT_EQUAL(UI_PIN_WEAK, result);
    TEST_ASSERT_FALSE_MESSAGE(ui_pin_is_configured(&s_storage),
        "Un PIN faible rejeté ne doit pas être stocké en NVS");
}

/* ========================================================================= */
/*                         Tests de vérification                              */
/* ========================================================================= */

/**
 * @brief Vérification d'un PIN correct.
 *
 * Scénario : on enregistre {7,3,9,1} puis on vérifie le même PIN.
 * Résultat attendu : ui_pin_verify retourne UI_PIN_OK.
 */
TEST_CASE("pin_verify_correct", "[ui_pin]")
{
    ui_pin_test_reset();

    const uint8_t pin[] = {7, 3, 9, 1};

    ui_pin_register(pin, &s_storage);
    ui_pin_result_t result = ui_pin_verify(pin, &s_storage);

    TEST_ASSERT_EQUAL(UI_PIN_OK, result);
}

/**
 * @brief Vérification d'un PIN incorrect.
 *
 * Scénario : on enregistre {7,3,9,1} puis on vérifie {9,8,7,6}.
 * Résultat attendu : ui_pin_verify retourne UI_PIN_WRONG.
 */
TEST_CASE("pin_verify_wrong", "[ui_pin]")
{
    ui_pin_test_reset();

    const uint8_t correct_pin[] = {7, 3, 9, 1};
    const uint8_t wrong_pin[]   = {9, 8, 7, 6};

    ui_pin_register(correct_pin, &s_storage);
    ui_pin_result_t result = ui_pin_verify(wrong_pin, &s_storage);

    TEST_ASSERT_EQUAL(UI_PIN_WRONG, result);
}

/* ========================================================================= */
/*                         Tests anti brute-force                             */
/* ========================================================================= */

/**
 * @brief Le compteur d'échecs s'incrémente à chaque mauvais PIN.
 *
 * Scénario : on enregistre un PIN puis on saisit 3 fois un PIN incorrect.
 * Résultat attendu : le compteur pin_fails dans le mock storage vaut 3.
 *
 * Note : les 3 premiers échecs n'entraînent pas encore de délai,
 * mais le compteur doit être fidèlement incrémenté.
 */
TEST_CASE("pin_verify_wrong_increments_counter", "[ui_pin]")
{
    ui_pin_test_reset();

    const uint8_t correct_pin[] = {7, 3, 9, 1};
    const uint8_t wrong_pin[]   = {9, 8, 7, 6};

    ui_pin_register(correct_pin, &s_storage);

    /* 3 tentatives incorrectes */
    for (int i = 0; i < 3; i++) {
        ui_pin_verify(wrong_pin, &s_storage);
    }

    /* Lecture directe du compteur dans le mock storage */
    uint32_t fail_count = 0;
    s_storage.u32_read("pin", "pin_fails", &fail_count, s_storage.ctx);

    TEST_ASSERT_EQUAL_UINT32(3, fail_count);
}

/**
 * @brief Un PIN correct remet le compteur d'échecs à zéro.
 *
 * Scénario : on enregistre un PIN, on saisit 2 fois un mauvais PIN,
 * puis on saisit le bon PIN.
 * Résultat attendu : le compteur pin_fails est remis à 0 après
 * la vérification correcte.
 */
TEST_CASE("pin_verify_correct_resets_counter", "[ui_pin]")
{
    ui_pin_test_reset();

    const uint8_t correct_pin[] = {7, 3, 9, 1};
    const uint8_t wrong_pin[]   = {9, 8, 7, 6};

    ui_pin_register(correct_pin, &s_storage);

    /* 2 échecs */
    ui_pin_verify(wrong_pin, &s_storage);
    ui_pin_verify(wrong_pin, &s_storage);

    /* Vérification correcte : doit remettre le compteur à 0 */
    ui_pin_result_t result = ui_pin_verify(correct_pin, &s_storage);
    TEST_ASSERT_EQUAL(UI_PIN_OK, result);

    /* Le compteur doit être à 0 (ou absent du storage) */
    uint32_t fail_count = 99;
    s_storage.u32_read("pin", "pin_fails", &fail_count, s_storage.ctx);

    TEST_ASSERT_EQUAL_UINT32(0, fail_count);
}

/**
 * @brief Le device se bloque après 10 échecs.
 *
 * Scénario : on simule 10 échecs en écrivant directement le compteur
 * dans le mock storage, puis on tente une vérification.
 * Résultat attendu : ui_pin_verify retourne UI_PIN_BLOCKED,
 * même avec le bon PIN.
 */
TEST_CASE("pin_blocked_after_10_failures", "[ui_pin]")
{
    ui_pin_test_reset();

    const uint8_t correct_pin[] = {7, 3, 9, 1};

    ui_pin_register(correct_pin, &s_storage);

    /* Injection directe de 10 échecs dans le storage */
    s_storage.u32_write("pin", "pin_fails", 10, s_storage.ctx);

    /* Même le bon PIN doit être rejeté : device bloqué */
    ui_pin_result_t result = ui_pin_verify(correct_pin, &s_storage);

    TEST_ASSERT_EQUAL_MESSAGE(UI_PIN_BLOCKED, result,
        "Après 10 échecs, le device doit être définitivement bloqué");
}

/**
 * @brief Cooldown redémarré si le timestamp NVS est futur.
 *
 * Scénario : un timestamp persisté en NVS provient d'une session
 * antérieure où esp_timer_get_time() avait une valeur élevée. Après
 * reboot, l'horloge monotone repart à zéro, mais le timestamp reste
 * en NVS. Sans correctif, now - last_fail est négatif, elapsed_s
 * passe sous le seuil, et le device reste verrouillé indéfiniment.
 *
 * Préparation : on enregistre un PIN, on injecte 3 échecs (seuil
 * cooldown 30s actif) et un last_fail = INT64_MAX (clairement
 * supérieur à toute valeur courante d'esp_timer_get_time()).
 *
 * Résultat attendu : la vérification avec le bon PIN doit retourner
 * UI_PIN_COOLDOWN. Depuis F-UI-007, un reboot pendant le délai ne permet
 * pas de contourner l'anti brute-force.
 */
TEST_CASE("pin_cooldown_stale_timestamp_after_reboot", "[ui_pin]")
{
    ui_pin_test_reset();

    const uint8_t correct_pin[] = {7, 3, 9, 1};

    ui_pin_register(correct_pin, &s_storage);

    /* Simuler 3 échecs persistés depuis une session précédente */
    s_storage.u32_write("pin", "pin_fails", 3, s_storage.ctx);

    /* Injecter un timestamp largement futur dans le blob pin_fail_t :
       impossible à atteindre par esp_timer_get_time() apres reboot. */
    int64_t stale = INT64_MAX;
    s_storage.blob_write("pin", "pin_fail_t",
                         (const uint8_t *)&stale, sizeof(stale),
                         s_storage.ctx);

    /* Bon PIN : le cooldown complet redémarre après reboot suspect. */
    ui_pin_result_t result = ui_pin_verify(correct_pin, &s_storage);
    TEST_ASSERT_EQUAL_MESSAGE(UI_PIN_COOLDOWN, result,
        "Un timestamp NVS futur doit redemarrer le cooldown anti-bypass");
}

/**
 * @brief ui_pin_cooldown_remaining() retourne le delai complet si le
 *        timestamp est futur.
 *
 * Variante de pin_cooldown_stale_timestamp_after_reboot ciblant
 * l'accesseur public utilisé par l'UI pour afficher le temps restant.
 */
TEST_CASE("pin_cooldown_remaining_stale_timestamp", "[ui_pin]")
{
    ui_pin_test_reset();

    const uint8_t correct_pin[] = {7, 3, 9, 1};

    ui_pin_register(correct_pin, &s_storage);

    s_storage.u32_write("pin", "pin_fails", 3, s_storage.ctx);
    int64_t stale = INT64_MAX;
    s_storage.blob_write("pin", "pin_fail_t",
                         (const uint8_t *)&stale, sizeof(stale),
                         s_storage.ctx);

    uint32_t remaining = ui_pin_cooldown_remaining(&s_storage);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(30, remaining,
        "Un timestamp NVS futur doit afficher le cooldown complet");
}

/* ========================================================================= */
/*                         Tests d'état du stockage                           */
/* ========================================================================= */

/**
 * @brief Aucun PIN configuré sur un storage vierge.
 *
 * Scénario : storage mock fraîchement créé, aucun enregistrement.
 * Résultat attendu : ui_pin_is_configured retourne false.
 */
TEST_CASE("pin_not_configured", "[ui_pin]")
{
    ui_pin_test_reset();

    TEST_ASSERT_FALSE(ui_pin_is_configured(&s_storage));
}

/**
 * @brief L'enregistrement d'un nouveau PIN écrase l'ancien.
 *
 * Scénario : on enregistre un PIN A ({7,3,9,1}), puis un PIN B ({5,8,2,6}).
 * Résultat attendu :
 *   - le PIN B est vérifié avec succès (UI_PIN_OK)
 *   - l'ancien PIN A ne fonctionne plus (UI_PIN_WRONG)
 */
TEST_CASE("pin_register_overwrites", "[ui_pin]")
{
    ui_pin_test_reset();

    const uint8_t pin_a[] = {7, 3, 9, 1};
    const uint8_t pin_b[] = {5, 8, 2, 6};

    /* Enregistrement initial puis remplacement */
    ui_pin_register(pin_a, &s_storage);
    ui_pin_register(pin_b, &s_storage);

    /* Le nouveau PIN doit fonctionner */
    ui_pin_result_t result_b = ui_pin_verify(pin_b, &s_storage);
    TEST_ASSERT_EQUAL(UI_PIN_OK, result_b);

    /* L'ancien PIN ne doit plus fonctionner */
    ui_pin_result_t result_a = ui_pin_verify(pin_a, &s_storage);
    TEST_ASSERT_EQUAL(UI_PIN_WRONG, result_a);
}
