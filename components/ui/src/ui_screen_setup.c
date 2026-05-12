/**
 * @file ui_screen_setup.c
 * @brief Ecran Setup initial — Configuration initiale du dispositif.
 *
 * Premier ecran affiche au demarrage quand aucun PIN n'est configure.
 * Permet a l'utilisateur de creer son code PIN pour securiser
 * les transactions. Apres la saisie du PIN, navigue vers HOME.
 *
 * Layouts :
 * - Grand ecran (CYD 320x240) : contenu centre avec marges genereueses
 * - Petit ecran (Waveshare 172x320) : layout vertical compact
 */

#include "ui/ui_screens.h"
#include "ui/ui_state.h"
#include "ui/ui_theme.h"
#include "ui/ui_manager.h"
#include "ui/ui_pin.h"

#include "esp_log.h"
#include <string.h>

static const char *TAG = "ui_setup";

/* Ecran racine */
static lv_obj_t *s_screen = NULL;

/* Contexte UI sauvegarde pour le callback PIN */
static ui_ctx_t *s_ctx = NULL;

/* ================================================================
 * Callbacks
 * ================================================================ */

/**
 * Callback appele quand l'utilisateur a saisi les 4 chiffres du PIN.
 *
 * Enregistre le PIN en NVS (hash SHA-256). Si le PIN est dans la
 * blacklist des codes faibles, ferme l'overlay et en reouvre un
 * avec un message d'erreur pour que l'utilisateur recommence.
 *
 * @param pin       Tableau de 4 chiffres (0-9)
 * @param user_data Non utilise
 */
static void pin_created_cb(const uint8_t pin[UI_PIN_LENGTH], void *user_data)
{
    (void)user_data;

    if (!s_ctx || !s_ctx->storage) {
        return;
    }

    ui_pin_result_t result = ui_pin_register(pin, s_ctx->storage);

    if (result == UI_PIN_WEAK) {
        ESP_LOGW(TAG, "PIN rejete (trop faible), nouvelle tentative");
        /* Fermer et reouvrir avec un titre d'erreur */
        ui_pin_close();
        ui_pin_show(NULL, "PIN trop simple !", pin_created_cb, NULL,
                    s_ctx->is_small_screen);
        return;
    }

    if (result != UI_PIN_OK) {
        ESP_LOGE(TAG, "Erreur enregistrement PIN");
        return;
    }

    ESP_LOGI(TAG, "PIN configure avec succes");

    /* Fermer l'overlay de saisie PIN */
    ui_pin_close();

    /* Naviguer vers l'ecran d'accueil */
    ui_manager_show(UI_SCREEN_HOME);
}

/**
 * Callback du bouton "Configurer PIN".
 * Ouvre l'overlay modal de saisie de PIN.
 */
static void configure_pin_cb(lv_event_t *e)
{
    (void)e;

    if (!s_ctx) {
        return;
    }

    /* Afficher l'ecran de saisie PIN en mode creation */
    ui_pin_show(NULL, "Nouveau PIN", pin_created_cb, NULL,
                s_ctx->is_small_screen);
}

/* ================================================================
 * Handler d'ecran
 * ================================================================ */

static lv_obj_t *screen_create(ui_ctx_t *ctx)
{
    s_ctx = ctx;

    s_screen = lv_obj_create(NULL);
    lv_obj_add_style(s_screen, &ui_style_screen, 0);

    /* Raccourcis pour les dimensions adaptatives */
    const bool small = ctx->is_small_screen;

    /* --- Conteneur principal centre --- */
    lv_obj_t *cont = lv_obj_create(s_screen);
    lv_obj_remove_style_all(cont);
    lv_obj_set_size(cont, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(cont, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(cont, small ? 12 : 24, 0);
    lv_obj_set_style_pad_row(cont, small ? 10 : 16, 0);

    /* --- Logo / Titre de l'application --- */
    lv_obj_t *title = lv_label_create(cont);
    lv_obj_add_style(title, &ui_style_title, 0);
    lv_label_set_text(title, "Offline Payment");
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);

    /* --- Sous-titre : nom de la monnaie --- */
    lv_obj_t *subtitle = lv_label_create(cont);
    lv_obj_add_style(subtitle, &ui_style_text, 0);
    lv_obj_set_style_text_color(subtitle, UI_COLOR_ACCENT, 0);
    lv_obj_set_style_text_align(subtitle, LV_TEXT_ALIGN_CENTER, 0);

    if (ctx->currency) {
        lv_label_set_text(subtitle, ctx->currency->name);
    } else {
        lv_label_set_text(subtitle, "---");
    }

    /* --- Texte informatif / bienvenue --- */
    lv_obj_t *info = lv_label_create(cont);
    lv_obj_add_style(info, &ui_style_text, 0);
    lv_obj_set_style_text_color(info, UI_COLOR_TEXT_DIM, 0);
    lv_obj_set_style_text_align(info, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(info, small ? LV_PCT(95) : LV_PCT(80));
    lv_label_set_long_mode(info, LV_LABEL_LONG_WRAP);
    lv_label_set_text(info,
        "Bienvenue ! Configurez votre\n"
        "code PIN pour securiser\n"
        "vos transactions.");

    /* --- Bouton "Configurer PIN" --- */
    lv_obj_t *btn = lv_button_create(cont);
    lv_obj_add_style(btn, &ui_style_btn, 0);
    lv_obj_set_size(btn, small ? LV_PCT(85) : 200, small ? 40 : 48);
    lv_obj_add_event_cb(btn, configure_pin_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *btn_label = lv_label_create(btn);
    lv_obj_set_style_text_color(btn_label, UI_COLOR_TEXT, 0);
    lv_obj_set_style_text_font(btn_label, ui_theme_font_normal(), 0);
    lv_label_set_text(btn_label, "Configurer PIN");
    lv_obj_center(btn_label);

    return s_screen;
}

static void screen_update(ui_ctx_t *ctx)
{
    /* Pas de donnees dynamiques sur l'ecran de setup */
    (void)ctx;
}

static void screen_destroy(void)
{
    if (s_screen) {
        lv_obj_delete(s_screen);
        s_screen = NULL;
    }
    s_ctx = NULL;
}

ui_screen_handler_t ui_screen_setup_handler = {
    .create  = screen_create,
    .update  = screen_update,
    .destroy = screen_destroy,
};
