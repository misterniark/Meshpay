/**
 * @file ui_screen_receive.c
 * @brief Ecran Reception paiement — Affichage de la cle publique et attente.
 *
 * Contenu :
 *   - Header avec titre "Recevoir" et bouton retour
 *   - Cle publique du device affichee en hexadecimal (tronquee, 2 lignes)
 *   - Alias du device
 *   - Message d'attente de paiement
 *
 * Layout adaptatif :
 *   - Grand ecran (320x240) : marges normales, police standard
 *   - Petit ecran (320x172 paysage) : carte large et basse, marges reduites
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

static lv_obj_t *s_screen      = NULL;
/** Label de la cle publique (mis a jour dans update) */
static lv_obj_t *s_key_label   = NULL;
/** Label de l'alias (mis a jour dans update) */
static lv_obj_t *s_alias_label = NULL;

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
 * Formate une cle publique en hexadecimal sur 2 lignes.
 * Ligne 1 : les 16 premiers octets
 * Ligne 2 : les 16 derniers octets
 *
 * @param buf      Buffer de sortie (doit faire au moins 80 octets)
 * @param buf_len  Taille du buffer
 * @param key      Cle publique a formater
 */
static void format_key_two_lines(char *buf, size_t buf_len,
                                 const public_key_t *key)
{
    char line1[48], line2[48];
    int pos = 0;

    /* Premiere moitie (16 octets) */
    for (int i = 0; i < 16; i++) {
        pos += snprintf(line1 + pos, sizeof(line1) - pos, "%02x", key->bytes[i]);
    }

    pos = 0;
    /* Seconde moitie (16 octets) */
    for (int i = 16; i < 32; i++) {
        pos += snprintf(line2 + pos, sizeof(line2) - pos, "%02x", key->bytes[i]);
    }

    snprintf(buf, buf_len, "%s\n%s", line1, line2);
}

/**
 * Formate une cle publique de maniere compacte (debut...fin).
 * Utilisee sur petit ecran pour economiser l'espace.
 *
 * @param buf      Buffer de sortie
 * @param buf_len  Taille du buffer
 * @param key      Cle publique a formater
 */
static void format_key_compact(char *buf, size_t buf_len,
                               const public_key_t *key)
{
    snprintf(buf, buf_len,
             "%02x%02x%02x%02x%02x%02x...\n...%02x%02x%02x%02x%02x%02x",
             key->bytes[0],  key->bytes[1],  key->bytes[2],
             key->bytes[3],  key->bytes[4],  key->bytes[5],
             key->bytes[26], key->bytes[27], key->bytes[28],
             key->bytes[29], key->bytes[30], key->bytes[31]);
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
     * Header : titre "Recevoir" + bouton retour
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
    lv_label_set_text(title, "Recevoir");
    lv_obj_align(title, LV_ALIGN_CENTER, 0, 0);

    /* ----------------------------------------------------------
     * Carte centrale avec les informations du device
     * ---------------------------------------------------------- */
    lv_coord_t card_y = small ? 44 : 52;

    lv_obj_t *card = lv_obj_create(s_screen);
    lv_obj_remove_style_all(card);
    lv_obj_add_style(card, &ui_style_card, 0);
    lv_obj_set_size(card, small ? 290 : 290, small ? 80 : 120);
    lv_obj_align(card, LV_ALIGN_TOP_MID, 0, card_y);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    /* Alias du device */
    s_alias_label = lv_label_create(card);
    lv_obj_add_style(s_alias_label, &ui_style_text, 0);
    lv_obj_set_style_text_font(s_alias_label, ui_theme_font_title(), 0);
    lv_obj_set_style_text_color(s_alias_label, UI_COLOR_TEXT, 0);
    lv_label_set_text(s_alias_label, "...");
    lv_obj_align(s_alias_label, LV_ALIGN_TOP_MID, 0, small ? 4 : 12);

    /* Label "Cle publique :" */
    lv_obj_t *key_title = lv_label_create(card);
    lv_obj_add_style(key_title, &ui_style_text, 0);
    lv_obj_set_style_text_color(key_title, UI_COLOR_TEXT_DIM, 0);
    lv_obj_set_style_text_font(key_title, ui_theme_font_normal(), 0);
    lv_label_set_text(key_title, "Cle publique :");
    lv_obj_align(key_title, LV_ALIGN_TOP_MID, 0, small ? 24 : 42);

    /* Cle publique en hexadecimal (2 lignes) */
    s_key_label = lv_label_create(card);
    lv_obj_add_style(s_key_label, &ui_style_text, 0);
    lv_obj_set_style_text_color(s_key_label, UI_COLOR_ACCENT, 0);
    lv_obj_set_style_text_font(s_key_label, ui_theme_font_normal(), 0);
    lv_obj_set_style_text_align(s_key_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(s_key_label, "...");
    lv_obj_align(s_key_label, LV_ALIGN_TOP_MID, 0, small ? 42 : 62);
    lv_obj_set_width(s_key_label, small ? 270 : 270);
    lv_label_set_long_mode(s_key_label, LV_LABEL_LONG_WRAP);

    /* ----------------------------------------------------------
     * Message d'attente en bas
     * ---------------------------------------------------------- */
    lv_obj_t *wait_label = lv_label_create(s_screen);
    lv_obj_add_style(wait_label, &ui_style_text, 0);
    lv_obj_set_style_text_color(wait_label, UI_COLOR_WARNING, 0);
    lv_obj_set_style_text_font(wait_label, ui_theme_font_normal(), 0);
    lv_obj_set_style_text_align(wait_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(wait_label, "En attente de\npaiement...");
    lv_obj_align(wait_label, LV_ALIGN_BOTTOM_MID, 0, small ? -8 : -30);

    return s_screen;
}

static void screen_update(ui_ctx_t *ctx)
{
    if (!s_screen) {
        return;
    }

    const bool small = ctx->is_small_screen;

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

        /* Mise a jour de la cle publique */
        if (ctx->own_pubkey) {
            char key_buf[96];
            if (small) {
                format_key_compact(key_buf, sizeof(key_buf),
                                   ctx->own_pubkey);
            } else {
                format_key_two_lines(key_buf, sizeof(key_buf),
                                     ctx->own_pubkey);
            }
            lv_label_set_text(s_key_label, key_buf);
        }

        xSemaphoreGive(ctx->state_mutex);
    }
}

static void screen_destroy(void)
{
    if (s_screen) {
        lv_obj_delete(s_screen);
        s_screen      = NULL;
        s_key_label   = NULL;
        s_alias_label = NULL;
    }
}

ui_screen_handler_t ui_screen_receive_handler = {
    .create  = screen_create,
    .update  = screen_update,
    .destroy = screen_destroy,
};
