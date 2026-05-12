/**
 * @file ui_screen_history.c
 * @brief Ecran Historique transactions — Liste des transactions passees.
 *
 * Contenu :
 *   - Header avec titre "Historique" et bouton retour
 *   - Liste scrollable des dernieres TX du DAG
 *   - Chaque TX : type (TRANSFER/MINT), montant, timestamp relatif
 *
 * Layout adaptatif :
 *   - Grand ecran (320x240) : 2 lignes par TX (type+montant, puis from/to tronque)
 *   - Petit ecran (172x320) : 1 ligne par TX (type + montant compact)
 *   - Si DAG vide : message "Aucune transaction"
 */

#include "ui/ui_screens.h"
#include "ui/ui_state.h"
#include "ui/ui_theme.h"
#include "ui/ui_manager.h"

#include <stdio.h>
#include <string.h>

/* Nombre maximum de TX affichees dans la liste */
#define HISTORY_MAX_DISPLAY 50

/* ================================================================
 * Variables statiques de l'ecran
 * ================================================================ */

static lv_obj_t *s_screen     = NULL;
/** Liste scrollable des transactions */
static lv_obj_t *s_tx_list    = NULL;
/** Label affiche quand le DAG est vide */
static lv_obj_t *s_empty_label = NULL;

/** Dernier nombre de TX connu, pour eviter de reconstruire inutilement */
static uint32_t s_last_count = UINT32_MAX;

/* ================================================================
 * Callbacks
 * ================================================================ */

/** Retour a l'ecran precedent */
static void back_cb(lv_event_t *e)
{
    (void)e;
    ui_manager_back();
}

/* ================================================================
 * Helpers
 * ================================================================ */

/**
 * Formate un timestamp relatif en texte lisible.
 * Ex : "il y a 2 min", "il y a 1h", "il y a 3j"
 *
 * @param buf       Buffer de sortie
 * @param buf_len   Taille du buffer
 * @param tx_ts_ms  Timestamp de la transaction (ms)
 * @param now_ms    Timestamp actuel (ms)
 */
static void format_relative_time(char *buf, size_t buf_len,
                                 uint64_t tx_ts_ms, uint64_t now_ms)
{
    if (tx_ts_ms >= now_ms || now_ms == 0) {
        snprintf(buf, buf_len, "maintenant");
        return;
    }

    uint64_t diff_sec = (now_ms - tx_ts_ms) / 1000;

    if (diff_sec < 60) {
        snprintf(buf, buf_len, "< 1 min");
    } else if (diff_sec < 3600) {
        snprintf(buf, buf_len, "%lu min", (unsigned long)(diff_sec / 60));
    } else if (diff_sec < 86400) {
        snprintf(buf, buf_len, "%luh", (unsigned long)(diff_sec / 3600));
    } else {
        snprintf(buf, buf_len, "%luj", (unsigned long)(diff_sec / 86400));
    }
}

/**
 * Formate une cle publique en hexadecimal court : "aabb..ccdd"
 */
static void format_key_short(char *buf, size_t buf_len,
                             const public_key_t *key)
{
    snprintf(buf, buf_len, "%02x%02x..%02x%02x",
             key->bytes[0],  key->bytes[1],
             key->bytes[30], key->bytes[31]);
}

/* ================================================================
 * Interface ecran
 * ================================================================ */

static lv_obj_t *screen_create(ui_ctx_t *ctx)
{
    s_screen = lv_obj_create(NULL);
    lv_obj_add_style(s_screen, &ui_style_screen, 0);
    s_last_count = UINT32_MAX; /* Force le premier rafraichissement */

    const bool small = ctx->is_small_screen;

    /* ----------------------------------------------------------
     * Header : titre "Historique" + bouton retour
     * ---------------------------------------------------------- */
    lv_obj_t *header = lv_obj_create(s_screen);
    lv_obj_remove_style_all(header);
    lv_obj_add_style(header, &ui_style_header, 0);
    lv_obj_set_size(header, lv_pct(100), small ? 36 : 40);
    lv_obj_align(header, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_clear_flag(header, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *btn_back = lv_button_create(header);
    lv_obj_set_size(btn_back, small ? 32 : 40, small ? 28 : 32);
    lv_obj_add_style(btn_back, &ui_style_btn, 0);
    lv_obj_align(btn_back, LV_ALIGN_LEFT_MID, small ? 4 : 8, 0);
    lv_obj_add_event_cb(btn_back, back_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *back_lbl = lv_label_create(btn_back);
    lv_label_set_text(back_lbl, LV_SYMBOL_LEFT);
    lv_obj_center(back_lbl);

    lv_obj_t *title = lv_label_create(header);
    lv_obj_add_style(title, &ui_style_title, 0);
    lv_label_set_text(title, "Historique");
    lv_obj_align(title, LV_ALIGN_CENTER, 0, 0);

    /* ----------------------------------------------------------
     * Label "Aucune transaction" (visible quand DAG vide)
     * ---------------------------------------------------------- */
    s_empty_label = lv_label_create(s_screen);
    lv_obj_add_style(s_empty_label, &ui_style_text, 0);
    lv_obj_set_style_text_color(s_empty_label, UI_COLOR_TEXT_DIM, 0);
    lv_obj_set_style_text_font(s_empty_label, ui_theme_font_normal(), 0);
    lv_label_set_text(s_empty_label, "Aucune transaction");
    lv_obj_align(s_empty_label, LV_ALIGN_CENTER, 0, 0);

    /* ----------------------------------------------------------
     * Liste scrollable des transactions
     * ---------------------------------------------------------- */
    lv_coord_t content_y = small ? 38 : 44;

    s_tx_list = lv_obj_create(s_screen);
    lv_obj_remove_style_all(s_tx_list);
    lv_obj_set_size(s_tx_list, lv_pct(100),
                    ctx->screen_h - content_y - 4);
    lv_obj_align(s_tx_list, LV_ALIGN_TOP_MID, 0, content_y);
    lv_obj_set_flex_flow(s_tx_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_tx_list, small ? 3 : 5, 0);
    lv_obj_set_style_pad_left(s_tx_list, small ? 4 : 8, 0);
    lv_obj_set_style_pad_right(s_tx_list, small ? 4 : 8, 0);
    lv_obj_set_style_pad_top(s_tx_list, 4, 0);
    lv_obj_add_flag(s_tx_list, LV_OBJ_FLAG_SCROLLABLE);

    return s_screen;
}

/**
 * Rafraichit la liste des transactions.
 *
 * Reconstruit les elements de la liste a partir du DAG.
 * Les TX sont affichees du plus recent au plus ancien
 * (parcours inverse du tableau).
 *
 * Optimisation : ne reconstruit que si le nombre de TX a change.
 */
static void screen_update(ui_ctx_t *ctx)
{
    if (!s_screen || !s_tx_list) {
        return;
    }

    const bool small = ctx->is_small_screen;

    if (xSemaphoreTake(ctx->state_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        uint32_t count = ctx->dag ? ctx->dag->count : 0;

        /* Optimisation : ne reconstruire que si le nombre de TX a change */
        if (count == s_last_count) {
            xSemaphoreGive(ctx->state_mutex);
            return;
        }
        s_last_count = count;

        /* Nettoyer la liste */
        lv_obj_clean(s_tx_list);

        if (count == 0) {
            /* DAG vide : afficher le message */
            lv_obj_clear_flag(s_empty_label, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_tx_list, LV_OBJ_FLAG_HIDDEN);
            xSemaphoreGive(ctx->state_mutex);
            return;
        }

        /* Des TX existent : masquer le message, afficher la liste */
        lv_obj_add_flag(s_empty_label, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_tx_list, LV_OBJ_FLAG_HIDDEN);

        /* Recuperer le timestamp actuel pour le calcul relatif */
        uint64_t now_ms = 0;
        if (ctx->wallet && ctx->wallet->get_time) {
            now_ms = ctx->wallet->get_time();
        }

        /* Nombre de TX a afficher (limite a HISTORY_MAX_DISPLAY) */
        uint32_t display_count = count;
        if (display_count > HISTORY_MAX_DISPLAY) {
            display_count = HISTORY_MAX_DISPLAY;
        }

        /* Parcours inverse : TX les plus recentes d'abord */
        for (uint32_t i = 0; i < display_count; i++) {
            uint32_t idx = count - 1 - i;
            const transaction_t *tx = &ctx->dag->transactions[idx];

            /* Determiner le type et la couleur */
            const char *type_str;
            lv_color_t type_color;
            if (tx->type == TX_TYPE_MINT) {
                type_str  = "MINT";
                type_color = UI_COLOR_SUCCESS;
            } else {
                type_str  = "TRANSFER";
                type_color = UI_COLOR_ACCENT;
            }

            /* Formater le timestamp relatif */
            char time_str[24];
            format_relative_time(time_str, sizeof(time_str),
                                 tx->timestamp, now_ms);

            if (small) {
                /* --------------------------------------------------
                 * Petit ecran : 1 ligne par TX
                 * Format : "TRANSFER  500  2 min"
                 * -------------------------------------------------- */
                lv_obj_t *row = lv_obj_create(s_tx_list);
                lv_obj_remove_style_all(row);
                lv_obj_add_style(row, &ui_style_card, 0);
                lv_obj_set_size(row, lv_pct(100), 30);
                lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

                /* Type */
                lv_obj_t *type_lbl = lv_label_create(row);
                lv_obj_set_style_text_font(type_lbl, ui_theme_font_normal(), 0);
                lv_obj_set_style_text_color(type_lbl, type_color, 0);
                lv_label_set_text(type_lbl, type_str);
                lv_obj_align(type_lbl, LV_ALIGN_LEFT_MID, 4, 0);

                /* Montant */
                char amount_str[16];
                snprintf(amount_str, sizeof(amount_str), "%lu",
                         (unsigned long)tx->amount);
                lv_obj_t *amount_lbl = lv_label_create(row);
                lv_obj_set_style_text_font(amount_lbl, ui_theme_font_normal(), 0);
                lv_obj_set_style_text_color(amount_lbl, UI_COLOR_TEXT, 0);
                lv_label_set_text(amount_lbl, amount_str);
                lv_obj_align(amount_lbl, LV_ALIGN_CENTER, 0, 0);

                /* Temps relatif */
                lv_obj_t *time_lbl = lv_label_create(row);
                lv_obj_set_style_text_font(time_lbl, ui_theme_font_normal(), 0);
                lv_obj_set_style_text_color(time_lbl, UI_COLOR_TEXT_DIM, 0);
                lv_label_set_text(time_lbl, time_str);
                lv_obj_align(time_lbl, LV_ALIGN_RIGHT_MID, -4, 0);

            } else {
                /* --------------------------------------------------
                 * Grand ecran : 2 lignes par TX
                 * Ligne 1 : type + montant + temps
                 * Ligne 2 : from -> to (cles tronquees)
                 * -------------------------------------------------- */
                lv_obj_t *card = lv_obj_create(s_tx_list);
                lv_obj_remove_style_all(card);
                lv_obj_add_style(card, &ui_style_card, 0);
                lv_obj_set_size(card, lv_pct(100), 48);
                lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

                /* Ligne 1 : type */
                lv_obj_t *type_lbl = lv_label_create(card);
                lv_obj_set_style_text_font(type_lbl, ui_theme_font_normal(), 0);
                lv_obj_set_style_text_color(type_lbl, type_color, 0);
                lv_label_set_text(type_lbl, type_str);
                lv_obj_align(type_lbl, LV_ALIGN_TOP_LEFT, 8, 4);

                /* Ligne 1 : montant */
                char amount_str[16];
                snprintf(amount_str, sizeof(amount_str), "%lu",
                         (unsigned long)tx->amount);
                lv_obj_t *amount_lbl = lv_label_create(card);
                lv_obj_set_style_text_font(amount_lbl, ui_theme_font_normal(), 0);
                lv_obj_set_style_text_color(amount_lbl, UI_COLOR_TEXT, 0);
                lv_label_set_text(amount_lbl, amount_str);
                lv_obj_align(amount_lbl, LV_ALIGN_TOP_MID, 0, 4);

                /* Ligne 1 : temps relatif */
                lv_obj_t *time_lbl = lv_label_create(card);
                lv_obj_set_style_text_font(time_lbl, ui_theme_font_normal(), 0);
                lv_obj_set_style_text_color(time_lbl, UI_COLOR_TEXT_DIM, 0);
                lv_label_set_text(time_lbl, time_str);
                lv_obj_align(time_lbl, LV_ALIGN_TOP_RIGHT, -8, 4);

                /* Ligne 2 : from -> to (cles tronquees) */
                char from_str[16], to_str[16], addr_line[48];
                format_key_short(from_str, sizeof(from_str), &tx->from);
                format_key_short(to_str,   sizeof(to_str),   &tx->to);
                snprintf(addr_line, sizeof(addr_line), "%s -> %s",
                         from_str, to_str);

                lv_obj_t *addr_lbl = lv_label_create(card);
                lv_obj_set_style_text_font(addr_lbl, ui_theme_font_normal(), 0);
                lv_obj_set_style_text_color(addr_lbl, UI_COLOR_TEXT_DIM, 0);
                lv_label_set_text(addr_lbl, addr_line);
                lv_obj_align(addr_lbl, LV_ALIGN_BOTTOM_LEFT, 8, -4);
            }
        }

        xSemaphoreGive(ctx->state_mutex);
    }
}

static void screen_destroy(void)
{
    if (s_screen) {
        lv_obj_delete(s_screen);
        s_screen      = NULL;
        s_tx_list     = NULL;
        s_empty_label = NULL;
        s_last_count  = UINT32_MAX;
    }
}

ui_screen_handler_t ui_screen_history_handler = {
    .create  = screen_create,
    .update  = screen_update,
    .destroy = screen_destroy,
};
