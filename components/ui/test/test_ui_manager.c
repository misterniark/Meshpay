/**
 * @file test_ui_manager.c
 * @brief Tests unitaires pour le gestionnaire d'écrans ui_manager.
 *
 * Couvre les bugs B1 (saturation de la pile de navigation) et B2
 * (handler manquant lors d'un back()) identifiés dans l'audit Lot A.
 *
 * Pour pouvoir tester sans dépendre de LVGL ni des écrans réels,
 * ce fichier fournit :
 *   - 13 handlers fictifs (un par ui_screen_id_t) qui remplacent au
 *     link les symboles attendus par ui_manager.c. Les create()
 *     retournent NULL pour court-circuiter l'appel à lv_screen_load().
 *   - Un stub de lv_screen_load() : ne sera jamais appelé en pratique
 *     (create() retourne NULL), mais il faut le symbole pour le link.
 *
 * En conséquence, ce test doit être bâti en isolation, sans linker
 * les vrais src/ui_screen_*.c ni la lib LVGL (duplications sinon).
 */

#include "unity.h"
#include "ui/ui_manager.h"
#include "ui/ui_screens.h"
#include <stddef.h>

/* ========================================================================= */
/*                         Stubs : LVGL + handlers d'écran                    */
/* ========================================================================= */

/**
 * Stub minimal de lv_screen_load() : non appelé tant que create()
 * retourne NULL, mais le symbole doit exister pour le linker.
 */
void lv_screen_load(lv_obj_t *scr)
{
    (void)scr;
}

/** Compteurs pour valider les invariants depuis les tests. */
static int s_create_count[UI_SCREEN_COUNT];
static int s_destroy_count[UI_SCREEN_COUNT];

/**
 * Macro de définition d'un handler fictif.
 *
 * @param name  Nom canonique de l'écran (correspond au symbole attendu
 *              par ui_manager.c : ui_screen_<name>_handler)
 * @param id    Valeur de ui_screen_id_t correspondante
 *
 * create() retourne NULL : ui_manager.c ne fera donc pas appel à
 * lv_screen_load(). destroy() n'est pas branchée volontairement
 * sur un compteur séparé pour rester proche du pattern de production
 * (handlers minimaux).
 */
#define DEFINE_STUB_HANDLER(name, id)                                       \
    static lv_obj_t *stub_create_##name(ui_ctx_t *ctx)                       \
    {                                                                       \
        (void)ctx;                                                          \
        s_create_count[id]++;                                               \
        return NULL;                                                        \
    }                                                                       \
    static void stub_destroy_##name(void)                                   \
    {                                                                       \
        s_destroy_count[id]++;                                              \
    }                                                                       \
    ui_screen_handler_t ui_screen_##name##_handler = {                       \
        .create  = stub_create_##name,                                      \
        .update  = NULL,                                                    \
        .destroy = stub_destroy_##name                                      \
    }

DEFINE_STUB_HANDLER(setup,     UI_SCREEN_SETUP);
DEFINE_STUB_HANDLER(home,      UI_SCREEN_HOME);
DEFINE_STUB_HANDLER(pay,       UI_SCREEN_PAY);
DEFINE_STUB_HANDLER(history,   UI_SCREEN_HISTORY);
DEFINE_STUB_HANDLER(settings,  UI_SCREEN_SETTINGS);
DEFINE_STUB_HANDLER(receive,   UI_SCREEN_RECEIVE);
DEFINE_STUB_HANDLER(broadcast, UI_SCREEN_BROADCAST);
DEFINE_STUB_HANDLER(admin,     UI_SCREEN_ADMIN);
DEFINE_STUB_HANDLER(mint,      UI_SCREEN_MINT);
DEFINE_STUB_HANDLER(message,   UI_SCREEN_MESSAGE);
DEFINE_STUB_HANDLER(scan,      UI_SCREEN_SCAN);
DEFINE_STUB_HANDLER(rename,    UI_SCREEN_RENAME);
DEFINE_STUB_HANDLER(forward,   UI_SCREEN_FORWARD);

/* ========================================================================= */
/*                         Fixtures                                           */
/* ========================================================================= */

/**
 * Compteurs remis à zéro avant chaque test, puis ui_manager_init pour
 * repartir d'un état propre (handlers réenregistrés, pile vide,
 * écran courant = HOME).
 */
__attribute__((weak)) void setUp(void)
{
    for (int i = 0; i < UI_SCREEN_COUNT; i++) {
        s_create_count[i]  = 0;
        s_destroy_count[i] = 0;
    }
    ui_manager_init(NULL); /* ctx NULL : les stubs ne le déréférencent pas */
}

__attribute__((weak)) void tearDown(void)
{
}

/* ========================================================================= */
/*                         Tests : navigation nominale                        */
/* ========================================================================= */

/**
 * @brief Après init, on est sur HOME et la pile est vide.
 */
TEST_CASE("ui_manager_initial_state", "[ui_manager]")
{
    TEST_ASSERT_EQUAL(UI_SCREEN_HOME, ui_manager_current());
    TEST_ASSERT_EQUAL_INT(0, ui_manager_nav_depth());
}

/**
 * @brief show() empile l'écran courant et active le nouveau.
 */
TEST_CASE("ui_manager_show_pushes_stack", "[ui_manager]")
{
    TEST_ASSERT_TRUE(ui_manager_show(UI_SCREEN_PAY));
    TEST_ASSERT_EQUAL(UI_SCREEN_PAY, ui_manager_current());
    TEST_ASSERT_EQUAL_INT(1, ui_manager_nav_depth());
    TEST_ASSERT_EQUAL_INT(1, s_create_count[UI_SCREEN_PAY]);
    TEST_ASSERT_EQUAL_INT(1, s_destroy_count[UI_SCREEN_HOME]);
}

/**
 * @brief back() depuis HOME (pile vide) retombe sur HOME, ne plante pas.
 */
TEST_CASE("ui_manager_back_empty_stack_returns_home", "[ui_manager]")
{
    TEST_ASSERT_TRUE(ui_manager_back());
    TEST_ASSERT_EQUAL(UI_SCREEN_HOME, ui_manager_current());
    TEST_ASSERT_EQUAL_INT(0, ui_manager_nav_depth());
}

/**
 * @brief Aller-retour HOME → PAY → HOME : la pile se vide correctement.
 */
TEST_CASE("ui_manager_show_then_back", "[ui_manager]")
{
    ui_manager_show(UI_SCREEN_PAY);
    TEST_ASSERT_TRUE(ui_manager_back());
    TEST_ASSERT_EQUAL(UI_SCREEN_HOME, ui_manager_current());
    TEST_ASSERT_EQUAL_INT(0, ui_manager_nav_depth());
}

/* ========================================================================= */
/*                         Tests : Bug B1 — pile saturée                      */
/* ========================================================================= */

/**
 * @brief show() refuse la navigation quand la pile est pleine (bug B1).
 *
 * Scénario : on empile UI_NAV_STACK_DEPTH écrans consécutifs, puis
 * on tente un show() supplémentaire.
 *
 * Résultat attendu :
 *   - le show() final retourne false ;
 *   - l'écran courant n'a pas changé ;
 *   - l'écran courant n'a pas été détruit (sinon on perdrait l'UI) ;
 *   - le nouvel écran n'a pas été créé.
 *
 * Avant le fix, l'ancien comportement empilait silencieusement sans
 * incrément, détruisait l'écran courant, et basculait sur le nouveau —
 * cassant le chemin de retour.
 */
TEST_CASE("ui_manager_show_refuses_when_stack_full", "[ui_manager]")
{
    /* On utilise des écrans alternés pour éviter qu'un meme id apparaisse
       deux fois et fausse les compteurs : ce test cible la pile, pas
       le contenu des entrees. */
    ui_screen_id_t fillers[] = {
        UI_SCREEN_PAY, UI_SCREEN_HISTORY, UI_SCREEN_SETTINGS,
        UI_SCREEN_RECEIVE, UI_SCREEN_BROADCAST, UI_SCREEN_ADMIN,
        UI_SCREEN_MINT, UI_SCREEN_MESSAGE,
    };
    /* On empile jusqu'à UI_NAV_STACK_DEPTH - 1 niveaux (la dernière
       case est l'écran courant, le reste est l'historique). */
    for (int i = 0; i < UI_NAV_STACK_DEPTH - 1; i++) {
        TEST_ASSERT_TRUE(ui_manager_show(fillers[i]));
    }
    /* Une show() de plus arrive en limite : doit etre acceptee
       (top passe a UI_NAV_STACK_DEPTH - 1, dernier slot dispo). */
    TEST_ASSERT_TRUE(ui_manager_show(fillers[UI_NAV_STACK_DEPTH - 1]));
    TEST_ASSERT_EQUAL_INT(UI_NAV_STACK_DEPTH, ui_manager_nav_depth());

    ui_screen_id_t cur_before = ui_manager_current();
    int created_before = s_create_count[UI_SCREEN_SCAN];

    /* Tentative supplementaire : doit etre refusee. */
    TEST_ASSERT_FALSE_MESSAGE(ui_manager_show(UI_SCREEN_SCAN),
        "show() doit retourner false quand la pile est saturee");

    /* Etat inchange : pas de bascule d'ecran, pas de destruction. */
    TEST_ASSERT_EQUAL(cur_before, ui_manager_current());
    TEST_ASSERT_EQUAL_INT(UI_NAV_STACK_DEPTH, ui_manager_nav_depth());
    TEST_ASSERT_EQUAL_INT_MESSAGE(created_before, s_create_count[UI_SCREEN_SCAN],
        "Le nouvel ecran ne doit pas avoir ete cree");
}

/* ========================================================================= */
/*                         Tests : Bug B2 — handler manquant                  */
/* ========================================================================= */

/* Pour simuler un handler absent on a besoin de pouvoir le retirer
   apres init. ui_manager.c n'expose pas d'API pour ca, mais on peut
   neutraliser un handler fictif (sa create devient NULL) via une
   astuce : on ecrase directement l'instance handler. Comme nos stubs
   sont des globals modifiables par le test, on peut les desactiver. */

/**
 * @brief back() avec un handler cible NULL retombe sur HOME (bug B2).
 *
 * Scénario : on empile PAY puis HISTORY (s_current = HISTORY,
 * pile = [HOME, PAY]). On retire la fonction create() de PAY pour
 * simuler une table de handlers incomplete. On appelle back().
 *
 * Résultat attendu :
 *   - back() retourne true (pas de crash) ;
 *   - l'écran courant est HOME (fallback) ;
 *   - HOME a bien été créé.
 *
 * Avant le fix, l'appel s_handlers[prev]->create plantait sur NULL.
 */
TEST_CASE("ui_manager_back_falls_back_to_home_if_handler_missing",
          "[ui_manager]")
{
    ui_manager_show(UI_SCREEN_PAY);
    ui_manager_show(UI_SCREEN_HISTORY);

    /* Sauvegarde + neutralisation du handler PAY pour simuler une
       table incoherente (par exemple un enum etendu sans handler). */
    ui_screen_handler_t saved = ui_screen_pay_handler;
    ui_screen_pay_handler.create  = NULL;
    ui_screen_pay_handler.destroy = NULL;

    int home_created_before = s_create_count[UI_SCREEN_HOME];

    bool ok = ui_manager_back();

    /* Restauration pour ne pas polluer les tests suivants. */
    ui_screen_pay_handler = saved;

    TEST_ASSERT_TRUE_MESSAGE(ok, "back() doit reussir via le fallback HOME");
    TEST_ASSERT_EQUAL_MESSAGE(UI_SCREEN_HOME, ui_manager_current(),
        "Le fallback doit ramener sur HOME");
    TEST_ASSERT_GREATER_THAN_INT(home_created_before,
                                  s_create_count[UI_SCREEN_HOME]);
}
