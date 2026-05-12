/**
 * @file ui_screen_broadcast.c
 * @brief Ecran Notification broadcast — Affichage d'un message broadcast recu.
 *
 * Ecran de type alerte plein ecran, style modal :
 * - Fond colore (UI_COLOR_WARNING)
 * - Texte du broadcast affiche en grand au centre
 * - Bouton "Fermer" en bas qui revient en arriere et reset broadcast_pending
 *
 * Cet ecran est affiche automatiquement quand un broadcast est recu.
 * Le texte est mis a jour dans screen_update() depuis ctx->pending_broadcast.
 */

#include "ui/ui_screens.h"
#include "ui/ui_state.h"
#include "ui/ui_theme.h"
#include "ui/ui_manager.h"

#include <string.h>

static lv_obj_t *s_screen    = NULL;
static lv_obj_t *s_lbl_text  = NULL; /* Label du texte broadcast */

/* ----------------------------------------------------------------
 * Callbacks
 * ---------------------------------------------------------------- */

/**
 * Callback du bouton "Fermer" :
 * - Remet broadcast_pending a false
 * - Retourne a l'ecran precedent
 */
static void close_cb(lv_event_t *e)
{
    ui_ctx_t *ctx = (ui_ctx_t *)lv_event_get_user_data(e);
    if (ctx && ctx->broadcast_pending) {
        *ctx->broadcast_pending = false;
    }
    ui_manager_back();
}

/* ----------------------------------------------------------------
 * Interface ecran
 * ---------------------------------------------------------------- */

static lv_obj_t *screen_create(ui_ctx_t *ctx)
{
    s_screen = lv_obj_create(NULL);

    /* Fond colore style alerte (couleur warning/orange) */
    lv_obj_set_style_bg_color(s_screen, UI_COLOR_WARNING, 0);
    lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(s_screen, ctx->is_small_screen ? 12 : 20, 0);

    /* Layout vertical centre */
    lv_obj_set_flex_flow(s_screen, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_screen, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    const bool small = ctx->is_small_screen;

    /* Icone / titre d'alerte */
    lv_obj_t *lbl_title = lv_label_create(s_screen);
    lv_obj_set_style_text_font(lbl_title, ui_theme_font_title(), 0);
    lv_obj_set_style_text_color(lbl_title, lv_color_hex(0x1A1A2E), 0);
    lv_label_set_text(lbl_title, LV_SYMBOL_WARNING " Broadcast");

    /* Texte du message broadcast, centre et adapte a la largeur */
    s_lbl_text = lv_label_create(s_screen);
    lv_obj_set_style_text_font(s_lbl_text, ui_theme_font_amount(), 0);
    lv_obj_set_style_text_color(s_lbl_text, lv_color_hex(0x1A1A2E), 0);
    lv_obj_set_style_text_align(s_lbl_text, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(s_lbl_text, small ? lv_pct(95) : lv_pct(85));
    lv_label_set_long_mode(s_lbl_text, LV_LABEL_LONG_WRAP);

    /* Afficher le texte initial s'il est disponible.
     * On force la terminaison null pour eviter un debordement
     * si le champ text n'est pas correctement termine. */
    if (ctx->pending_broadcast && ctx->broadcast_pending &&
        *ctx->broadcast_pending) {
        ctx->pending_broadcast->text[COMM_MSG_BROADCAST_TEXT_MAX] = '\0';
        lv_label_set_text(s_lbl_text, ctx->pending_broadcast->text);
    } else {
        lv_label_set_text(s_lbl_text, "...");
    }

    /* Espacement avant le bouton */
    lv_obj_set_style_pad_row(s_screen, small ? 6 : 24, 0);

    /* Bouton "Fermer" */
    lv_obj_t *btn_close = lv_button_create(s_screen);
    lv_obj_add_style(btn_close, &ui_style_btn, 0);
    lv_obj_set_size(btn_close, small ? lv_pct(60) : lv_pct(40),
                    small ? 36 : 44);
    lv_obj_set_style_bg_color(btn_close, lv_color_hex(0x1A1A2E), 0);
    lv_obj_add_event_cb(btn_close, close_cb, LV_EVENT_CLICKED, ctx);

    lv_obj_t *btn_label = lv_label_create(btn_close);
    lv_obj_set_style_text_font(btn_label, ui_theme_font_normal(), 0);
    lv_obj_set_style_text_color(btn_label, UI_COLOR_TEXT, 0);
    lv_label_set_text(btn_label, "Fermer");
    lv_obj_center(btn_label);

    return s_screen;
}

static void screen_update(ui_ctx_t *ctx)
{
    if (!s_screen || !s_lbl_text || !ctx) {
        return;
    }

    /* Prendre le mutex pour lire le texte du broadcast */
    if (xSemaphoreTake(ctx->state_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return;
    }

    if (ctx->pending_broadcast && ctx->broadcast_pending &&
        *ctx->broadcast_pending) {
        /* Forcer la terminaison null avant affichage */
        ctx->pending_broadcast->text[COMM_MSG_BROADCAST_TEXT_MAX] = '\0';
        lv_label_set_text(s_lbl_text, ctx->pending_broadcast->text);
    }

    xSemaphoreGive(ctx->state_mutex);
}

static void screen_destroy(void)
{
    if (s_screen) {
        lv_obj_delete(s_screen);
        s_screen   = NULL;
    }
    s_lbl_text = NULL;
}

ui_screen_handler_t ui_screen_broadcast_handler = {
    .create  = screen_create,
    .update  = screen_update,
    .destroy = screen_destroy,
};
