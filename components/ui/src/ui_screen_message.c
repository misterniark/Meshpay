/**
 * @file ui_screen_message.c
 * @brief Ecran Envoyer broadcast texte — Saisie et envoi d'un message diffuse.
 *
 * Permet au maitre de composer et envoyer un message broadcast texte
 * a tous les devices du reseau.
 *
 * Contenu :
 * - Header avec titre "Broadcast" et bouton retour
 * - Zone de saisie (lv_textarea)
 * - Compteur de caracteres "XX/157" avec alerte > 128 chars
 * - Bouton "Envoyer" qui poste UI_CMD_BROADCAST_TEXT
 */

#include "ui/ui_screens.h"
#include "ui/ui_state.h"
#include "ui/ui_theme.h"
#include "ui/ui_manager.h"

#include "comm/comm_msg.h"

#include <string.h>
#include <stdio.h>

static lv_obj_t *s_screen       = NULL;
static lv_obj_t *s_textarea     = NULL; /* Zone de saisie du texte */
static lv_obj_t *s_lbl_counter  = NULL; /* Compteur "XX/157" */
static lv_obj_t *s_btn_send     = NULL; /* Bouton envoyer */
static lv_obj_t *s_keyboard     = NULL; /* Clavier virtuel */

/** Seuil a partir duquel le compteur passe en couleur warning */
#define CHAR_WARNING_THRESHOLD 128

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
 * Callback appele a chaque changement de texte dans la textarea.
 * Met a jour le compteur de caracteres et change la couleur
 * si le seuil d'avertissement est depasse.
 */
static void textarea_changed_cb(lv_event_t *e)
{
    (void)e;
    if (!s_textarea || !s_lbl_counter) {
        return;
    }

    const char *text = lv_textarea_get_text(s_textarea);
    uint32_t len = (uint32_t)strlen(text);

    /* Mettre a jour le compteur */
    char buf[16];
    snprintf(buf, sizeof(buf), "%lu/%d", (unsigned long)len,
             COMM_MSG_BROADCAST_TEXT_MAX);
    lv_label_set_text(s_lbl_counter, buf);

    /* Couleur du compteur selon le seuil */
    if (len > CHAR_WARNING_THRESHOLD) {
        lv_obj_set_style_text_color(s_lbl_counter, UI_COLOR_WARNING, 0);
    } else {
        lv_obj_set_style_text_color(s_lbl_counter, UI_COLOR_TEXT_DIM, 0);
    }

    /* Desactiver le bouton si le texte est vide ou trop long */
    if (len == 0 || len > COMM_MSG_BROADCAST_TEXT_MAX) {
        lv_obj_add_state(s_btn_send, LV_STATE_DISABLED);
        lv_obj_set_style_opa(s_btn_send, LV_OPA_50, LV_STATE_DISABLED);
    } else {
        lv_obj_clear_state(s_btn_send, LV_STATE_DISABLED);
        lv_obj_set_style_opa(s_btn_send, LV_OPA_COVER, 0);
    }
}

/**
 * Callback du bouton "Envoyer" : poste la commande UI_CMD_BROADCAST_TEXT.
 *
 * Copie le texte de la textarea dans la commande et l'envoie
 * dans la queue (sans prendre le mutex).
 */
static void send_cb(lv_event_t *e)
{
    ui_ctx_t *ctx = (ui_ctx_t *)lv_event_get_user_data(e);
    if (!ctx || !s_textarea) {
        return;
    }

    const char *text = lv_textarea_get_text(s_textarea);
    uint32_t len = (uint32_t)strlen(text);

    /* Validation : texte non vide et dans la limite */
    if (len == 0 || len > COMM_MSG_BROADCAST_TEXT_MAX) {
        return;
    }

    /* Construire et poster la commande */
    ui_cmd_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.type = UI_CMD_BROADCAST_TEXT;
    memcpy(cmd.data.broadcast.text, text, len);
    cmd.data.broadcast.text[len] = '\0';
    cmd.data.broadcast.text_len = (uint8_t)len;
    xQueueSend(ctx->cmd_queue, &cmd, pdMS_TO_TICKS(100));

    /* Vider la textarea apres envoi et revenir en arriere */
    lv_textarea_set_text(s_textarea, "");
    ui_manager_back();
}

/* ----------------------------------------------------------------
 * Interface ecran
 * ---------------------------------------------------------------- */

static lv_obj_t *screen_create(ui_ctx_t *ctx)
{
    s_screen = lv_obj_create(NULL);
    lv_obj_add_style(s_screen, &ui_style_screen, 0);

    const bool small = ctx->is_small_screen;

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
    lv_label_set_text(title, "Broadcast");
    lv_obj_set_style_pad_left(title, 8, 0);

    /* --- Zone de saisie de texte --- */
    const int32_t header_h = small ? 40 : 48;
    const int32_t kb_h     = small ? 130 : 140;

    s_textarea = lv_textarea_create(s_screen);
    lv_obj_set_size(s_textarea, lv_pct(92),
                    small ? 60 : 80);
    lv_obj_align(s_textarea, LV_ALIGN_TOP_MID, 0, header_h + 4);
    lv_textarea_set_max_length(s_textarea, COMM_MSG_BROADCAST_TEXT_MAX);
    lv_textarea_set_placeholder_text(s_textarea, "Votre message...");
    lv_obj_set_style_text_font(s_textarea, ui_theme_font_normal(), 0);
    lv_obj_set_style_bg_color(s_textarea, UI_COLOR_CARD, 0);
    lv_obj_set_style_text_color(s_textarea, UI_COLOR_TEXT, 0);
    lv_obj_set_style_border_color(s_textarea, UI_COLOR_ACCENT, LV_STATE_FOCUSED);

    /* Callback de changement de texte pour le compteur */
    lv_obj_add_event_cb(s_textarea, textarea_changed_cb,
                        LV_EVENT_VALUE_CHANGED, NULL);

    /* --- Ligne : compteur + bouton envoyer --- */
    lv_obj_t *row = lv_obj_create(s_screen);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_size(row, lv_pct(92), small ? 34 : 40);
    lv_obj_align_to(row, s_textarea, LV_ALIGN_OUT_BOTTOM_MID, 0, 4);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(row, 0, 0);

    /* Compteur de caracteres */
    s_lbl_counter = lv_label_create(row);
    lv_obj_set_style_text_font(s_lbl_counter, ui_theme_font_normal(), 0);
    lv_obj_set_style_text_color(s_lbl_counter, UI_COLOR_TEXT_DIM, 0);
    lv_label_set_text_fmt(s_lbl_counter, "0/%d", COMM_MSG_BROADCAST_TEXT_MAX);

    /* Bouton "Envoyer" */
    s_btn_send = lv_button_create(row);
    lv_obj_add_style(s_btn_send, &ui_style_btn, 0);
    lv_obj_set_size(s_btn_send, small ? 90 : 110, small ? 30 : 36);
    lv_obj_set_style_bg_color(s_btn_send, UI_COLOR_ACCENT, 0);
    lv_obj_add_event_cb(s_btn_send, send_cb, LV_EVENT_CLICKED, ctx);

    /* Desactive par defaut (texte vide) */
    lv_obj_add_state(s_btn_send, LV_STATE_DISABLED);
    lv_obj_set_style_opa(s_btn_send, LV_OPA_50, LV_STATE_DISABLED);

    lv_obj_t *send_label = lv_label_create(s_btn_send);
    lv_obj_set_style_text_font(send_label, ui_theme_font_normal(), 0);
    lv_obj_set_style_text_color(send_label, UI_COLOR_TEXT, 0);
    lv_label_set_text(send_label, "Envoyer");
    lv_obj_center(send_label);

    /* --- Clavier virtuel --- */
    s_keyboard = lv_keyboard_create(s_screen);
    lv_obj_set_size(s_keyboard, lv_pct(100), kb_h);
    lv_obj_align(s_keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_textarea(s_keyboard, s_textarea);

    return s_screen;
}

static void screen_update(ui_ctx_t *ctx)
{
    /* Pas de donnees dynamiques a rafraichir sur cet ecran */
    (void)ctx;
}

static void screen_destroy(void)
{
    if (s_screen) {
        lv_obj_delete(s_screen);
        s_screen = NULL;
    }
    s_textarea    = NULL;
    s_lbl_counter = NULL;
    s_btn_send    = NULL;
    s_keyboard    = NULL;
}

ui_screen_handler_t ui_screen_message_handler = {
    .create  = screen_create,
    .update  = screen_update,
    .destroy = screen_destroy,
};
