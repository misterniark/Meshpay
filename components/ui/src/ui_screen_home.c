/**
 * @file ui_screen_home.c
 * @brief Ecran Accueil / Solde — Affichage du solde et acces rapide.
 *
 * Layout grand ecran (320x240 paysage) :
 *   - Haut : alias + nom de la monnaie
 *   - Centre : solde en grand (style amount)
 *   - Bas : 4 boutons en ligne (Payer, Recevoir, Historique, Parametres)
 *
 * Layout petit ecran (320x172 paysage) :
 *   - Haut : alias + nom de la monnaie (compact)
 *   - Centre : solde en grand
 *   - Bas : 4 boutons en ligne (compacts)
 */

#include "ui/ui_screens.h"
#include "ui/ui_state.h"
#include "ui/ui_theme.h"
#include "ui/ui_manager.h"

#include <stdio.h>
#include <string.h>

/* ================================================================
 * Variables statiques de l'ecran
 * ================================================================ */

static lv_obj_t *s_screen       = NULL;

/** Labels mis a jour dans screen_update() */
static lv_obj_t *s_balance_label = NULL;
static lv_obj_t *s_alias_label   = NULL;

/* ================================================================
 * Callbacks de navigation
 * ================================================================ */

static void nav_pay_cb(lv_event_t *e)
{
    (void)e;
    ui_manager_show(UI_SCREEN_PAY);
}

static void nav_receive_cb(lv_event_t *e)
{
    (void)e;
    ui_manager_show(UI_SCREEN_RECEIVE);
}

static void nav_history_cb(lv_event_t *e)
{
    (void)e;
    ui_manager_show(UI_SCREEN_HISTORY);
}

static void nav_settings_cb(lv_event_t *e)
{
    (void)e;
    ui_manager_show(UI_SCREEN_SETTINGS);
}

/* ================================================================
 * Helpers
 * ================================================================ */

/**
 * Formate le solde en chaine lisible avec les decimales.
 * Ex : amount=1500, decimals=2 -> "15.00"
 *      amount=1500, decimals=0 -> "1500"
 *
 * @param buf     Buffer de sortie
 * @param buf_len Taille du buffer
 * @param amount  Montant brut
 * @param decimals Nombre de decimales
 * @param symbol  Symbole de la monnaie
 */
static void format_balance(char *buf, size_t buf_len, uint32_t amount,
                           uint8_t decimals, const char *symbol)
{
    if (decimals == 0) {
        snprintf(buf, buf_len, "%lu %s", (unsigned long)amount, symbol);
    } else {
        /* Divise en partie entiere et decimale */
        uint32_t divisor = 1;
        for (uint8_t i = 0; i < decimals; i++) {
            divisor *= 10;
        }
        uint32_t integer_part  = amount / divisor;
        uint32_t decimal_part  = amount % divisor;
        snprintf(buf, buf_len, "%lu.%0*lu %s",
                 (unsigned long)integer_part,
                 (int)decimals,
                 (unsigned long)decimal_part,
                 symbol);
    }
}

/**
 * Cree un bouton de navigation avec un label centre.
 *
 * @param parent  Conteneur parent
 * @param text    Texte du bouton
 * @param w       Largeur du bouton
 * @param h       Hauteur du bouton
 * @param cb      Callback au clic
 * @return Pointeur vers le bouton cree
 */
static lv_obj_t *create_nav_btn(lv_obj_t *parent, const char *text,
                                lv_coord_t w, lv_coord_t h,
                                lv_event_cb_t cb)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_add_style(btn, &ui_style_btn, 0);
    lv_obj_set_size(btn, w, h);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *label = lv_label_create(btn);
    lv_obj_set_style_text_font(label, ui_theme_font_normal(), 0);
    lv_obj_set_style_text_color(label, UI_COLOR_TEXT, 0);
    lv_label_set_text(label, text);
    lv_obj_center(label);

    return btn;
}

/* ================================================================
 * Interface ecran
 * ================================================================ */

static lv_obj_t *screen_create(ui_ctx_t *ctx)
{
    s_screen = lv_obj_create(NULL);
    lv_obj_add_style(s_screen, &ui_style_screen, 0);

    const bool small = ctx->is_small_screen;

    /* ----------------------------------------------------------
     * Zone haute : alias du device + nom de la monnaie
     * ---------------------------------------------------------- */
    s_alias_label = lv_label_create(s_screen);
    lv_obj_add_style(s_alias_label, &ui_style_text, 0);
    lv_obj_set_style_text_color(s_alias_label, UI_COLOR_TEXT_DIM, 0);
    lv_obj_set_style_text_font(s_alias_label, ui_theme_font_normal(), 0);
    lv_label_set_text(s_alias_label, "...");
    lv_obj_align(s_alias_label, LV_ALIGN_TOP_MID, 0, small ? 8 : 12);

    lv_obj_t *currency_label = lv_label_create(s_screen);
    lv_obj_add_style(currency_label, &ui_style_text, 0);
    lv_obj_set_style_text_color(currency_label, UI_COLOR_TEXT_DIM, 0);
    lv_obj_set_style_text_font(currency_label, ui_theme_font_normal(), 0);
    lv_label_set_text(currency_label, ctx->currency ? ctx->currency->name : "---");
    lv_obj_align_to(currency_label, s_alias_label, LV_ALIGN_OUT_BOTTOM_MID,
                    0, small ? 2 : 4);

    /* ----------------------------------------------------------
     * Zone centrale : solde en grand
     * ---------------------------------------------------------- */
    s_balance_label = lv_label_create(s_screen);
    lv_obj_add_style(s_balance_label, &ui_style_amount, 0);
    lv_obj_set_style_text_font(s_balance_label, ui_theme_font_amount(), 0);
    lv_label_set_text(s_balance_label, "---");
    lv_obj_align(s_balance_label, LV_ALIGN_CENTER, 0, small ? -20 : -10);

    /* ----------------------------------------------------------
     * Zone basse : boutons de navigation
     *
     * Grand ecran : 4 boutons en ligne horizontale
     * Petit ecran : grille 2x2
     * ---------------------------------------------------------- */
    if (small) {
        /* Petit ecran paysage (320x172) : 4 boutons en ligne compacts */
        lv_coord_t btn_w = 70;
        lv_coord_t btn_h = 34;

        lv_obj_t *bar = lv_obj_create(s_screen);
        lv_obj_remove_style_all(bar);
        lv_obj_set_size(bar, 310, btn_h + 8);
        lv_obj_align(bar, LV_ALIGN_BOTTOM_MID, 0, -6);
        lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_SPACE_EVENLY,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

        create_nav_btn(bar, "Payer",    btn_w, btn_h, nav_pay_cb);
        create_nav_btn(bar, "Recevoir", btn_w, btn_h, nav_receive_cb);
        create_nav_btn(bar, "Histo.",   btn_w, btn_h, nav_history_cb);
        create_nav_btn(bar, "Param.",   btn_w, btn_h, nav_settings_cb);
    } else {
        /* Grand ecran (320x240) : 4 boutons en ligne */
        lv_coord_t btn_w = 70;
        lv_coord_t btn_h = 40;

        /* Conteneur flex horizontal */
        lv_obj_t *bar = lv_obj_create(s_screen);
        lv_obj_remove_style_all(bar);
        lv_obj_set_size(bar, 310, btn_h + 10);
        lv_obj_align(bar, LV_ALIGN_BOTTOM_MID, 0, -10);
        lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_SPACE_EVENLY,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

        create_nav_btn(bar, "Payer",    btn_w, btn_h, nav_pay_cb);
        create_nav_btn(bar, "Recevoir", btn_w, btn_h, nav_receive_cb);
        create_nav_btn(bar, "Histo.",   btn_w, btn_h, nav_history_cb);
        create_nav_btn(bar, "Param.",   btn_w, btn_h, nav_settings_cb);
    }

    return s_screen;
}

static void screen_update(ui_ctx_t *ctx)
{
    if (!s_screen) {
        return;
    }

    /* Lecture des donnees partagees sous mutex */
    if (xSemaphoreTake(ctx->state_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        /* Mise a jour de l'alias */
        if (ctx->device_alias && ctx->device_alias_len) {
            char alias_buf[64];
            uint8_t len = *ctx->device_alias_len;
            if (len > sizeof(alias_buf) - 1) {
                len = sizeof(alias_buf) - 1;
            }
            memcpy(alias_buf, ctx->device_alias, len);
            alias_buf[len] = '\0';
            lv_label_set_text(s_alias_label, alias_buf);
        }

        /*
         * Mise a jour du solde.
         * [C3-fix] On utilise get_owner_balance() (checkpoint + DAG)
         * au lieu de wallet_get_balance avec initial_balance en base.
         * L'ancien code double-comptait le solde initial : une TX MINT
         * pour initial_balance est creee au premier boot ET initial_balance
         * etait repasse en base, produisant 2 * initial_balance.
         */
        if (ctx->currency && ctx->get_owner_balance) {
            uint32_t balance = ctx->get_owner_balance();

            /* Appliquer la fonte pour l'affichage (lecture seule) */
            if (ctx->compute_melted_balance) {
                balance = ctx->compute_melted_balance(balance);
            }

            char buf[48];
            format_balance(buf, sizeof(buf), balance,
                           ctx->currency->decimals,
                           ctx->currency->symbol);
            lv_label_set_text(s_balance_label, buf);
        }

        xSemaphoreGive(ctx->state_mutex);
    }
}

static void screen_destroy(void)
{
    if (s_screen) {
        lv_obj_delete(s_screen);
        s_screen        = NULL;
        s_balance_label = NULL;
        s_alias_label   = NULL;
    }
}

ui_screen_handler_t ui_screen_home_handler = {
    .create  = screen_create,
    .update  = screen_update,
    .destroy = screen_destroy,
};
