/**
 * @file ui_screen_scan.c
 * @brief Ecran Scanner les devices — Recherche des peripheriques a proximite.
 *
 * Permet au maitre de lancer un scan (ping) et d'afficher les resultats.
 *
 * Contenu :
 * - Header avec titre "Scanner" et bouton retour
 * - Bouton "Lancer le scan" : poste UI_CMD_PING
 * - Indicateur "Scan en cours..." (label anime)
 * - Liste des resultats (alias + cle tronquee)
 * - Message "Aucun device trouve" si la liste est vide
 *
 * screen_update() rafraichit la liste quand ping_result_count change.
 */

#include "ui/ui_screens.h"
#include "ui/ui_state.h"
#include "ui/ui_theme.h"
#include "ui/ui_manager.h"

#include <string.h>
#include <stdio.h>

static lv_obj_t *s_screen         = NULL;
static lv_obj_t *s_list           = NULL; /* Liste des resultats */
static lv_obj_t *s_lbl_scanning   = NULL; /* Label "Scan en cours..." */
static lv_obj_t *s_lbl_empty      = NULL; /* Label "Aucun device trouve" */
static lv_obj_t *s_btn_scan       = NULL; /* Bouton lancer le scan */

/** Dernier nombre de resultats connu pour detecter les changements */
static uint32_t s_last_count = 0;

/** Flag indiquant qu'un scan est en cours */
static bool s_scanning = false;

/* ----------------------------------------------------------------
 * Callbacks
 * ---------------------------------------------------------------- */

/** Retour a l'ecran precedent */
static void back_cb(lv_event_t *e)
{
    (void)e;
    ui_manager_back();
}

/**
 * Callback du bouton "Lancer le scan" :
 * - Poste la commande UI_CMD_PING (sans mutex)
 * - Affiche l'indicateur "Scan en cours..."
 */
static void scan_cb(lv_event_t *e)
{
    ui_ctx_t *ctx = (ui_ctx_t *)lv_event_get_user_data(e);
    if (!ctx) {
        return;
    }

    /* Poster la commande de ping */
    ui_cmd_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.type = UI_CMD_PING;
    xQueueSend(ctx->cmd_queue, &cmd, pdMS_TO_TICKS(100));

    /* Afficher l'indicateur de scan */
    s_scanning = true;
    if (s_lbl_scanning) {
        lv_obj_clear_flag(s_lbl_scanning, LV_OBJ_FLAG_HIDDEN);
    }
}

/* ----------------------------------------------------------------
 * Utilitaires
 * ---------------------------------------------------------------- */

/**
 * Reconstruit la liste des resultats de ping.
 *
 * Vide la liste existante et la remplit avec les resultats actuels.
 * Chaque element affiche l'alias et les 4 premiers octets hex de la cle.
 *
 * @param ctx   Contexte UI (donnees deja sous mutex)
 * @param small true si petit ecran
 */
static void rebuild_result_list(ui_ctx_t *ctx, bool small)
{
    if (!s_list) {
        return;
    }

    /* Vider la liste actuelle */
    lv_obj_clean(s_list);

    uint32_t count = 0;
    if (ctx->ping_results && ctx->ping_result_count) {
        count = *ctx->ping_result_count;
    }

    if (count == 0) {
        /* Afficher le message "aucun device" */
        if (s_lbl_empty) {
            lv_obj_clear_flag(s_lbl_empty, LV_OBJ_FLAG_HIDDEN);
        }
        return;
    }

    /* Masquer le message "aucun device" */
    if (s_lbl_empty) {
        lv_obj_add_flag(s_lbl_empty, LV_OBJ_FLAG_HIDDEN);
    }

    /* Ajouter chaque resultat comme element de liste */
    for (uint32_t i = 0; i < count; i++) {
        const ui_ping_result_t *r = &ctx->ping_results[i];

        /* Formater : "alias  (AABB...CCDD)" avec les 4 premiers octets hex */
        char text[64];
        snprintf(text, sizeof(text), "%.*s  (%02X%02X..%02X%02X)",
                 r->alias_len, r->alias,
                 r->key.bytes[0], r->key.bytes[1],
                 r->key.bytes[30], r->key.bytes[31]);

        lv_obj_t *item = lv_list_add_text(s_list, text);
        lv_obj_set_style_text_font(item, ui_theme_font_normal(), 0);
        lv_obj_set_style_text_color(item, UI_COLOR_TEXT, 0);
        (void)small;
    }
}

/* ----------------------------------------------------------------
 * Interface ecran
 * ---------------------------------------------------------------- */

static lv_obj_t *screen_create(ui_ctx_t *ctx)
{
    s_screen = lv_obj_create(NULL);
    lv_obj_add_style(s_screen, &ui_style_screen, 0);

    const bool small = ctx->is_small_screen;
    s_scanning = false;
    s_last_count = 0;

    /* --- Header : bouton retour + titre --- */
    lv_obj_t *header = lv_obj_create(s_screen);
    lv_obj_add_style(header, &ui_style_header, 0);
    lv_obj_set_size(header, lv_pct(100), small ? 36 : 44);
    lv_obj_align(header, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(header, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_hor(header, 8, 0);

    lv_obj_t *btn_back = lv_button_create(header);
    lv_obj_set_size(btn_back, small ? 40 : 50, small ? 26 : 32);
    lv_obj_add_style(btn_back, &ui_style_btn, 0);
    lv_obj_add_event_cb(btn_back, back_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *back_label = lv_label_create(btn_back);
    lv_label_set_text(back_label, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_color(back_label, UI_COLOR_TEXT, 0);
    lv_obj_center(back_label);

    lv_obj_t *title = lv_label_create(header);
    lv_obj_add_style(title, &ui_style_title, 0);
    lv_label_set_text(title, "Scanner");
    lv_obj_set_style_pad_left(title, 8, 0);

    const int32_t header_h = small ? 40 : 48;

    /* --- Bouton "Lancer le scan" --- */
    s_btn_scan = lv_button_create(s_screen);
    lv_obj_add_style(s_btn_scan, &ui_style_btn, 0);
    lv_obj_set_size(s_btn_scan, small ? lv_pct(80) : lv_pct(50),
                    small ? 36 : 44);
    lv_obj_align(s_btn_scan, LV_ALIGN_TOP_MID, 0, header_h + 6);
    lv_obj_set_style_bg_color(s_btn_scan, UI_COLOR_ACCENT, 0);
    lv_obj_add_event_cb(s_btn_scan, scan_cb, LV_EVENT_CLICKED, ctx);

    lv_obj_t *scan_label = lv_label_create(s_btn_scan);
    lv_obj_set_style_text_font(scan_label, ui_theme_font_normal(), 0);
    lv_obj_set_style_text_color(scan_label, UI_COLOR_TEXT, 0);
    lv_label_set_text(scan_label, "Lancer le scan");
    lv_obj_center(scan_label);

    /* --- Indicateur "Scan en cours..." (masque par defaut) --- */
    s_lbl_scanning = lv_label_create(s_screen);
    lv_obj_set_style_text_font(s_lbl_scanning, ui_theme_font_normal(), 0);
    lv_obj_set_style_text_color(s_lbl_scanning, UI_COLOR_WARNING, 0);
    lv_label_set_text(s_lbl_scanning, "Scan en cours...");
    lv_obj_align_to(s_lbl_scanning, s_btn_scan, LV_ALIGN_OUT_BOTTOM_MID, 0, 6);
    lv_obj_add_flag(s_lbl_scanning, LV_OBJ_FLAG_HIDDEN);

    /* --- Label "Aucun device trouve" (masque par defaut) --- */
    s_lbl_empty = lv_label_create(s_screen);
    lv_obj_add_style(s_lbl_empty, &ui_style_text, 0);
    lv_obj_set_style_text_color(s_lbl_empty, UI_COLOR_TEXT_DIM, 0);
    lv_label_set_text(s_lbl_empty, "Aucun device trouve");
    lv_obj_align(s_lbl_empty, LV_ALIGN_CENTER, 0, small ? 20 : 30);
    /* Visible par defaut (sera masque quand des resultats arrivent) */

    /* --- Liste des resultats de scan --- */
    const int32_t list_top = header_h + (small ? 80 : 100);
    s_list = lv_list_create(s_screen);
    lv_obj_set_size(s_list, lv_pct(95),
                    ctx->screen_h - list_top - 8);
    lv_obj_align(s_list, LV_ALIGN_TOP_MID, 0, list_top);
    lv_obj_set_style_bg_color(s_list, UI_COLOR_CARD, 0);
    lv_obj_set_style_border_width(s_list, 0, 0);
    lv_obj_set_style_pad_all(s_list, small ? 2 : 4, 0);

    return s_screen;
}

static void screen_update(ui_ctx_t *ctx)
{
    if (!s_screen || !ctx) {
        return;
    }

    /* Prendre le mutex pour lire les resultats de ping */
    if (xSemaphoreTake(ctx->state_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return;
    }

    uint32_t current_count = 0;
    if (ctx->ping_results && ctx->ping_result_count) {
        current_count = *ctx->ping_result_count;
    }

    /* Detecter un changement dans le nombre de resultats */
    if (current_count != s_last_count) {
        s_last_count = current_count;

        /* Masquer l'indicateur de scan (les resultats sont arrives) */
        if (s_scanning && current_count > 0) {
            s_scanning = false;
            if (s_lbl_scanning) {
                lv_obj_add_flag(s_lbl_scanning, LV_OBJ_FLAG_HIDDEN);
            }
        }

        /* Reconstruire la liste des resultats */
        rebuild_result_list(ctx, ctx->is_small_screen);
    }

    xSemaphoreGive(ctx->state_mutex);
}

static void screen_destroy(void)
{
    if (s_screen) {
        lv_obj_delete(s_screen);
        s_screen = NULL;
    }
    s_list         = NULL;
    s_lbl_scanning = NULL;
    s_lbl_empty    = NULL;
    s_btn_scan     = NULL;
    s_last_count   = 0;
    s_scanning     = false;
}

ui_screen_handler_t ui_screen_scan_handler = {
    .create  = screen_create,
    .update  = screen_update,
    .destroy = screen_destroy,
};
