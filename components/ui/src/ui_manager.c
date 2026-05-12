/**
 * @file ui_manager.c
 * @brief Gestionnaire d'ecrans et navigation.
 *
 * Maintient une pile de navigation pour permettre le retour
 * a l'ecran precedent. Chaque ecran est cree/detruit a la volee
 * pour economiser la RAM (important sur ESP32 classique).
 *
 * Garanties d'integrite (post-audit Lot A) :
 *   - show() refuse la navigation si la pile est saturee : on prefere
 *     bloquer un drill-down plutot que perdre silencieusement le chemin
 *     de retour (bug B1 du rapport d'audit).
 *   - back() tolere un handler manquant en se rabattant sur HOME
 *     (bug B2 du rapport d'audit) : evite un crash si la table de
 *     handlers est incomplete ou si l'enum a derive.
 */

#include "ui/ui_manager.h"
#include "esp_log.h"

static const char *TAG = "ui_mgr";

/* Forward declarations des handlers d'ecrans */
extern ui_screen_handler_t ui_screen_setup_handler;
extern ui_screen_handler_t ui_screen_home_handler;
extern ui_screen_handler_t ui_screen_pay_handler;
extern ui_screen_handler_t ui_screen_history_handler;
extern ui_screen_handler_t ui_screen_settings_handler;
extern ui_screen_handler_t ui_screen_receive_handler;
extern ui_screen_handler_t ui_screen_broadcast_handler;
extern ui_screen_handler_t ui_screen_admin_handler;
extern ui_screen_handler_t ui_screen_mint_handler;
extern ui_screen_handler_t ui_screen_message_handler;
extern ui_screen_handler_t ui_screen_scan_handler;
extern ui_screen_handler_t ui_screen_rename_handler;
extern ui_screen_handler_t ui_screen_forward_handler;

/* ================================================================
 * Etat interne
 * ================================================================ */

/** Table des handlers, indexee par ui_screen_id_t */
static ui_screen_handler_t *s_handlers[UI_SCREEN_COUNT];

/** Pile de navigation. s_nav_top == -1 signifie pile vide. */
static ui_screen_id_t s_nav_stack[UI_NAV_STACK_DEPTH];
static int            s_nav_top = -1;

/** Ecran courant */
static ui_screen_id_t s_current = UI_SCREEN_HOME;

/** Contexte UI partage */
static ui_ctx_t *s_ctx = NULL;

/* ================================================================
 * API
 * ================================================================ */

void ui_manager_init(ui_ctx_t *ctx)
{
    s_ctx = ctx;

    /* Enregistrer tous les handlers */
    s_handlers[UI_SCREEN_SETUP]     = &ui_screen_setup_handler;
    s_handlers[UI_SCREEN_HOME]      = &ui_screen_home_handler;
    s_handlers[UI_SCREEN_PAY]       = &ui_screen_pay_handler;
    s_handlers[UI_SCREEN_HISTORY]   = &ui_screen_history_handler;
    s_handlers[UI_SCREEN_SETTINGS]  = &ui_screen_settings_handler;
    s_handlers[UI_SCREEN_RECEIVE]   = &ui_screen_receive_handler;
    s_handlers[UI_SCREEN_BROADCAST] = &ui_screen_broadcast_handler;
    s_handlers[UI_SCREEN_ADMIN]     = &ui_screen_admin_handler;
    s_handlers[UI_SCREEN_MINT]      = &ui_screen_mint_handler;
    s_handlers[UI_SCREEN_MESSAGE]   = &ui_screen_message_handler;
    s_handlers[UI_SCREEN_SCAN]      = &ui_screen_scan_handler;
    s_handlers[UI_SCREEN_RENAME]    = &ui_screen_rename_handler;
    s_handlers[UI_SCREEN_FORWARD]   = &ui_screen_forward_handler;

    /* Reset explicite — utile si init() est rappele apres un test */
    s_nav_top = -1;
    s_current = UI_SCREEN_HOME;

    ESP_LOGI(TAG, "UI Manager initialise (%d ecrans)", UI_SCREEN_COUNT);
}

bool ui_manager_show(ui_screen_id_t screen_id)
{
    /* Validation de l'identifiant et de la table de handlers */
    if (screen_id >= UI_SCREEN_COUNT || !s_handlers[screen_id]
        || !s_handlers[screen_id]->create) {
        ESP_LOGW(TAG, "Ecran inconnu ou handler invalide : %d", screen_id);
        return false;
    }

    /* [Fix B1] Refuser la navigation si la pile de retour est pleine
       AVANT de detruire l'ecran courant. Detruire d'abord puis ne pas
       pouvoir empiler conduirait a une perte de l'ecran courant et
       casserait le chemin de retour silencieusement. */
    if (s_nav_top >= UI_NAV_STACK_DEPTH - 1) {
        ESP_LOGE(TAG, "Pile de navigation saturee (%d) — show(%d) refuse",
                 UI_NAV_STACK_DEPTH, screen_id);
        return false;
    }

    /* Detruire l'ecran courant si un handler existe */
    if (s_handlers[s_current] && s_handlers[s_current]->destroy) {
        s_handlers[s_current]->destroy();
    }

    /* Empiler l'ecran courant pour le retour. La verification ci-dessus
       garantit que ++s_nav_top reste dans les bornes. */
    s_nav_stack[++s_nav_top] = s_current;

    /* Creer le nouvel ecran */
    s_current = screen_id;
    lv_obj_t *scr = s_handlers[screen_id]->create(s_ctx);

    if (scr) {
        lv_screen_load(scr);
    }

    ESP_LOGI(TAG, "Ecran %d affiche", screen_id);
    return true;
}

bool ui_manager_back(void)
{
    /* Detruire l'ecran courant */
    if (s_handlers[s_current] && s_handlers[s_current]->destroy) {
        s_handlers[s_current]->destroy();
    }

    /* Depiler l'ecran precedent. Pile vide => fallback sur HOME. */
    ui_screen_id_t prev = UI_SCREEN_HOME;
    if (s_nav_top >= 0) {
        prev = s_nav_stack[s_nav_top--];
    }

    /* [Fix B2] Garde : le handler peut etre absent si la table n'a pas
       ete completement initialisee ou si une valeur invalide a ete
       empilee. On retombe sur HOME comme dernier recours. Sans ce
       garde, l'appel s_handlers[prev]->create plantait sur deref NULL. */
    if (!s_handlers[prev] || !s_handlers[prev]->create) {
        ESP_LOGE(TAG, "Handler ecran %d absent — fallback HOME", prev);
        prev = UI_SCREEN_HOME;
        if (!s_handlers[prev] || !s_handlers[prev]->create) {
            /* Cas extreme : meme HOME est absent. On ne peut rien faire
               sans risquer un crash. On laisse s_current inchange. */
            ESP_LOGE(TAG, "Handler HOME absent — navigation impossible");
            return false;
        }
    }

    /* Creer l'ecran precedent */
    s_current = prev;
    lv_obj_t *scr = s_handlers[prev]->create(s_ctx);

    if (scr) {
        lv_screen_load(scr);
    }

    ESP_LOGI(TAG, "Retour ecran %d", prev);
    return true;
}

void ui_manager_update(void)
{
    if (s_handlers[s_current] && s_handlers[s_current]->update) {
        s_handlers[s_current]->update(s_ctx);
    }
}

ui_screen_id_t ui_manager_current(void)
{
    return s_current;
}

int ui_manager_nav_depth(void)
{
    /* s_nav_top == -1 (pile vide) => profondeur 0. */
    return s_nav_top + 1;
}
