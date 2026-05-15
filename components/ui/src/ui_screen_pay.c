/**
 * @file ui_screen_pay.c
 * @brief Ecran Payer — Selection d'un peer et envoi de paiement.
 *
 * Flux utilisateur :
 *   1. L'ecran affiche la liste des peers decouverts (scrollable)
 *   2. L'utilisateur selectionne un peer (carte avec alias + cle tronquee)
 *   3. Un champ montant apparait avec un spinbox
 *   4. "Envoyer" poste UI_CMD_PAY sur cmd_queue
 *
 * Un bouton "Decouvrir" permet de relancer la decouverte ESP-NOW.
 * Si aucun peer n'est trouve, un message informatif est affiche.
 *
 * Layout adaptatif :
 *   - Grand ecran (320x240) : cartes peers plus larges, marges normales
 *   - Petit ecran (172x320) : cartes compactes, marges reduites
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

static lv_obj_t *s_screen = NULL;

/** Conteneur scrollable pour la liste des peers */
static lv_obj_t *s_peer_list    = NULL;
/** Label affiche quand aucun peer n'est trouve */
static lv_obj_t *s_empty_label  = NULL;
/** Panneau de saisie du montant (visible apres selection d'un peer) */
static lv_obj_t *s_amount_panel = NULL;
/** Spinbox pour la saisie du montant */
static lv_obj_t *s_spinbox      = NULL;
/** Label du peer selectionne dans le panneau montant */
static lv_obj_t *s_selected_label = NULL;

/** Cle publique du peer selectionne (copie locale) */
static public_key_t s_selected_peer;
/** Indique si un peer est selectionne */
static bool s_peer_selected = false;

/** Reference vers le contexte UI (pour les callbacks) */
static ui_ctx_t *s_ctx = NULL;

/* ================================================================
 * Callbacks
 * ================================================================ */

/** Retour a l'ecran precedent */
static void back_cb(lv_event_t *e)
{
    (void)e;
    ui_manager_back();
}

/**
 * Callback quand un peer est selectionne dans la liste.
 * Copie la cle du peer et affiche le panneau de saisie du montant.
 */
static void peer_selected_cb(lv_event_t *e)
{
    /* L'index du peer est stocke dans user_data */
    uint32_t idx = (uint32_t)(uintptr_t)lv_event_get_user_data(e);

    if (!s_ctx) {
        return;
    }

    /* Lecture de la cle du peer sous mutex */
    if (xSemaphoreTake(s_ctx->state_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        if (s_ctx->peers && s_ctx->peer_count && idx < *s_ctx->peer_count) {
            s_selected_peer = s_ctx->peers[idx].public_key;
            s_peer_selected = true;

            /* Afficher l'alias du peer selectionne */
            if (s_selected_label) {
                char buf[48];
                snprintf(buf, sizeof(buf), "Vers: %s",
                         s_ctx->peers[idx].alias[0] ? s_ctx->peers[idx].alias : "???");
                lv_label_set_text(s_selected_label, buf);
            }
        }
        xSemaphoreGive(s_ctx->state_mutex);
    }

    /* Afficher le panneau de saisie du montant, masquer la liste */
    if (s_amount_panel) {
        lv_obj_clear_flag(s_amount_panel, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_peer_list) {
        lv_obj_add_flag(s_peer_list, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_empty_label) {
        lv_obj_add_flag(s_empty_label, LV_OBJ_FLAG_HIDDEN);
    }
}

/**
 * Callback du bouton "Envoyer" : poste la commande UI_CMD_PAY.
 * Le montant est lu depuis le spinbox.
 */
static void send_cb(lv_event_t *e)
{
    (void)e;
    if (!s_ctx || !s_peer_selected || !s_spinbox) {
        return;
    }

    int32_t amount = lv_spinbox_get_value(s_spinbox);
    if (amount <= 0) {
        return;
    }

    /* Construction et envoi de la commande (sans mutex) */
    ui_cmd_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.type = UI_CMD_PAY;
    cmd.data.pay.to     = s_selected_peer;
    cmd.data.pay.amount = (uint32_t)amount;
    xQueueSend(s_ctx->cmd_queue, &cmd, pdMS_TO_TICKS(100));

    /* Retour a l'ecran precedent apres envoi */
    ui_manager_back();
}

/**
 * Callback du bouton "Decouvrir" : lance une decouverte de peers.
 */
static void discover_cb(lv_event_t *e)
{
    (void)e;
    if (!s_ctx) {
        return;
    }

    ui_cmd_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.type = UI_CMD_DISCOVER_PEERS;
    xQueueSend(s_ctx->cmd_queue, &cmd, pdMS_TO_TICKS(100));
}

/**
 * Callback du bouton "+" du spinbox : incremente la valeur.
 * Le spinbox est passe en user_data.
 */
static void spinbox_inc_cb(lv_event_t *e)
{
    lv_obj_t *spinbox = lv_event_get_user_data(e);
    lv_spinbox_increment(spinbox);
}

/**
 * Callback du bouton "-" du spinbox : decremente la valeur.
 * Le spinbox est passe en user_data.
 */
static void spinbox_dec_cb(lv_event_t *e)
{
    lv_obj_t *spinbox = lv_event_get_user_data(e);
    lv_spinbox_decrement(spinbox);
}

/**
 * Callback du bouton "Annuler" dans le panneau montant :
 * revient a la liste des peers.
 */
static void cancel_amount_cb(lv_event_t *e)
{
    (void)e;
    s_peer_selected = false;

    if (s_amount_panel) {
        lv_obj_add_flag(s_amount_panel, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_peer_list) {
        lv_obj_clear_flag(s_peer_list, LV_OBJ_FLAG_HIDDEN);
    }
}

/* ================================================================
 * Helpers
 * ================================================================ */

/**
 * Formate une cle publique en hexadecimal tronque.
 * Affiche les 4 premiers octets et les 4 derniers : "aabb...ccdd"
 */
static void format_key_short(char *buf, size_t buf_len,
                             const public_key_t *key)
{
    snprintf(buf, buf_len, "%02x%02x%02x%02x...%02x%02x%02x%02x",
             key->bytes[0],  key->bytes[1],  key->bytes[2],  key->bytes[3],
             key->bytes[28], key->bytes[29], key->bytes[30], key->bytes[31]);
}

/* ================================================================
 * Interface ecran
 * ================================================================ */

static lv_obj_t *screen_create(ui_ctx_t *ctx)
{
    s_screen = lv_obj_create(NULL);
    lv_obj_add_style(s_screen, &ui_style_screen, 0);
    s_ctx = ctx;
    s_peer_selected = false;

    /*
     * [Audit 2026-05-15] Refresh automatique de la liste des peers
     * à chaque entrée sur l'écran Payer. Sans ça, l'utilisateur voyait
     * la liste figée du dernier DISCOVER (potentiellement vide ou
     * stale) et devait penser à cliquer manuellement sur "Découvrir".
     *
     * Le broadcast ANNOUNCE arrive de manière asynchrone (qq centaines
     * de ms) — l'utilisateur voit d'abord la liste précédente puis
     * elle se met à jour quand les peers répondent. La file
     * cmd_queue a un timeout de 100 ms : si elle est pleine, on
     * laisse tomber silencieusement (le bouton manuel "Découvrir"
     * reste disponible comme fallback).
     */
    if (ctx && ctx->cmd_queue) {
        ui_cmd_t auto_disc;
        memset(&auto_disc, 0, sizeof(auto_disc));
        auto_disc.type = UI_CMD_DISCOVER_PEERS;
        xQueueSend(ctx->cmd_queue, &auto_disc, pdMS_TO_TICKS(100));
    }

    const bool small = ctx->is_small_screen;

    /* ----------------------------------------------------------
     * Header : titre "Payer" + bouton retour
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
    lv_label_set_text(title, "Payer");
    lv_obj_align(title, LV_ALIGN_CENTER, 0, 0);

    /* Bouton "Decouvrir" a droite du header */
    lv_obj_t *btn_disc = lv_button_create(header);
    lv_obj_set_size(btn_disc, small ? 60 : 80, small ? 28 : 32);
    lv_obj_add_style(btn_disc, &ui_style_btn, 0);
    lv_obj_align(btn_disc, LV_ALIGN_RIGHT_MID, small ? -4 : -8, 0);
    lv_obj_add_event_cb(btn_disc, discover_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *disc_lbl = lv_label_create(btn_disc);
    lv_obj_set_style_text_font(disc_lbl, ui_theme_font_normal(), 0);
    lv_obj_set_style_text_color(disc_lbl, UI_COLOR_TEXT, 0);
    lv_label_set_text(disc_lbl, LV_SYMBOL_REFRESH);
    lv_obj_center(disc_lbl);

    /* ----------------------------------------------------------
     * Zone contenu : hauteur = ecran - header
     * ---------------------------------------------------------- */
    lv_coord_t content_y = small ? 38 : 44;

    /* ----------------------------------------------------------
     * Label "Aucun device trouve" (visible quand pas de peers)
     * ---------------------------------------------------------- */
    s_empty_label = lv_label_create(s_screen);
    lv_obj_add_style(s_empty_label, &ui_style_text, 0);
    lv_obj_set_style_text_color(s_empty_label, UI_COLOR_TEXT_DIM, 0);
    lv_obj_set_style_text_font(s_empty_label, ui_theme_font_normal(), 0);
    lv_label_set_text(s_empty_label, "Aucun device trouve");
    lv_obj_align(s_empty_label, LV_ALIGN_CENTER, 0, 0);

    /* ----------------------------------------------------------
     * Liste scrollable des peers
     * ---------------------------------------------------------- */
    s_peer_list = lv_obj_create(s_screen);
    lv_obj_remove_style_all(s_peer_list);
    lv_obj_set_size(s_peer_list, lv_pct(100),
                    ctx->screen_h - content_y - 10);
    lv_obj_align(s_peer_list, LV_ALIGN_TOP_MID, 0, content_y);
    lv_obj_set_flex_flow(s_peer_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_peer_list, small ? 4 : 6, 0);
    lv_obj_set_style_pad_left(s_peer_list, small ? 4 : 8, 0);
    lv_obj_set_style_pad_right(s_peer_list, small ? 4 : 8, 0);
    lv_obj_set_style_pad_top(s_peer_list, 4, 0);
    lv_obj_add_flag(s_peer_list, LV_OBJ_FLAG_SCROLLABLE);

    /* ----------------------------------------------------------
     * Panneau de saisie du montant (masque au depart)
     * ---------------------------------------------------------- */
    s_amount_panel = lv_obj_create(s_screen);
    lv_obj_remove_style_all(s_amount_panel);
    lv_obj_set_size(s_amount_panel, lv_pct(100),
                    ctx->screen_h - content_y - 10);
    lv_obj_align(s_amount_panel, LV_ALIGN_TOP_MID, 0, content_y);
    lv_obj_clear_flag(s_amount_panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_amount_panel, LV_OBJ_FLAG_HIDDEN);

    /* Label du peer selectionne */
    s_selected_label = lv_label_create(s_amount_panel);
    lv_obj_add_style(s_selected_label, &ui_style_text, 0);
    lv_obj_set_style_text_font(s_selected_label, ui_theme_font_normal(), 0);
    lv_label_set_text(s_selected_label, "Vers: ---");
    lv_obj_align(s_selected_label, LV_ALIGN_TOP_MID, 0, small ? 8 : 16);

    /* Label "Montant" */
    lv_obj_t *amount_title = lv_label_create(s_amount_panel);
    lv_obj_add_style(amount_title, &ui_style_text, 0);
    lv_obj_set_style_text_color(amount_title, UI_COLOR_TEXT_DIM, 0);
    lv_obj_set_style_text_font(amount_title, ui_theme_font_normal(), 0);
    lv_label_set_text(amount_title, "Montant :");
    lv_obj_align(amount_title, LV_ALIGN_TOP_MID, 0, small ? 36 : 48);

    /* Spinbox pour la saisie du montant */
    s_spinbox = lv_spinbox_create(s_amount_panel);
    lv_spinbox_set_range(s_spinbox, 1, 999999);
    lv_spinbox_set_digit_format(s_spinbox, 6, 0);
    lv_spinbox_set_value(s_spinbox, 1);
    lv_obj_set_width(s_spinbox, small ? 120 : 160);
    lv_obj_set_style_text_font(s_spinbox, ui_theme_font_amount(), 0);
    lv_obj_set_style_text_color(s_spinbox, UI_COLOR_TEXT, 0);
    lv_obj_set_style_bg_color(s_spinbox, UI_COLOR_CARD, 0);
    lv_obj_set_style_bg_opa(s_spinbox, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_spinbox, UI_COLOR_ACCENT, 0);
    lv_obj_set_style_border_width(s_spinbox, 2, 0);
    lv_obj_align(s_spinbox, LV_ALIGN_CENTER, 0, small ? -10 : -5);

    /* Boutons increment/decrement du spinbox */
    lv_coord_t inc_btn_w = small ? 36 : 44;
    lv_coord_t inc_btn_h = small ? 32 : 36;

    lv_obj_t *btn_inc = lv_button_create(s_amount_panel);
    lv_obj_set_size(btn_inc, inc_btn_w, inc_btn_h);
    lv_obj_add_style(btn_inc, &ui_style_btn, 0);
    lv_obj_align_to(btn_inc, s_spinbox, LV_ALIGN_OUT_RIGHT_MID, 6, 0);
    lv_obj_add_event_cb(btn_inc, spinbox_inc_cb,
                        LV_EVENT_SHORT_CLICKED, s_spinbox);
    lv_obj_t *inc_lbl = lv_label_create(btn_inc);
    lv_label_set_text(inc_lbl, LV_SYMBOL_PLUS);
    lv_obj_center(inc_lbl);

    lv_obj_t *btn_dec = lv_button_create(s_amount_panel);
    lv_obj_set_size(btn_dec, inc_btn_w, inc_btn_h);
    lv_obj_add_style(btn_dec, &ui_style_btn, 0);
    lv_obj_align_to(btn_dec, s_spinbox, LV_ALIGN_OUT_LEFT_MID, -6, 0);
    lv_obj_add_event_cb(btn_dec, spinbox_dec_cb,
                        LV_EVENT_SHORT_CLICKED, s_spinbox);
    lv_obj_t *dec_lbl = lv_label_create(btn_dec);
    lv_label_set_text(dec_lbl, LV_SYMBOL_MINUS);
    lv_obj_center(dec_lbl);

    /* Boutons "Envoyer" et "Annuler" en bas du panneau */
    lv_coord_t action_btn_w = small ? 70 : 100;
    lv_coord_t action_btn_h = small ? 36 : 40;

    lv_obj_t *btn_send = lv_button_create(s_amount_panel);
    lv_obj_set_size(btn_send, action_btn_w, action_btn_h);
    lv_obj_add_style(btn_send, &ui_style_btn, 0);
    lv_obj_set_style_bg_color(btn_send, UI_COLOR_SUCCESS, 0);
    lv_obj_align(btn_send, LV_ALIGN_BOTTOM_RIGHT, small ? -10 : -20,
                 small ? -10 : -16);
    lv_obj_add_event_cb(btn_send, send_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *send_lbl = lv_label_create(btn_send);
    lv_obj_set_style_text_font(send_lbl, ui_theme_font_normal(), 0);
    lv_obj_set_style_text_color(send_lbl, UI_COLOR_TEXT, 0);
    lv_label_set_text(send_lbl, "Envoyer");
    lv_obj_center(send_lbl);

    lv_obj_t *btn_cancel = lv_button_create(s_amount_panel);
    lv_obj_set_size(btn_cancel, action_btn_w, action_btn_h);
    lv_obj_add_style(btn_cancel, &ui_style_btn, 0);
    lv_obj_align(btn_cancel, LV_ALIGN_BOTTOM_LEFT, small ? 10 : 20,
                 small ? -10 : -16);
    lv_obj_add_event_cb(btn_cancel, cancel_amount_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *cancel_lbl = lv_label_create(btn_cancel);
    lv_obj_set_style_text_font(cancel_lbl, ui_theme_font_normal(), 0);
    lv_obj_set_style_text_color(cancel_lbl, UI_COLOR_TEXT, 0);
    lv_label_set_text(cancel_lbl, "Annuler");
    lv_obj_center(cancel_lbl);

    return s_screen;
}

/**
 * Rafraichit la liste des peers decouverts.
 *
 * Reconstruit les cartes peer dans le conteneur scrollable.
 * Chaque carte affiche l'alias et la cle publique tronquee du peer.
 */
static void screen_update(ui_ctx_t *ctx)
{
    if (!s_screen || !s_peer_list) {
        return;
    }

    /* Si le panneau montant est visible, pas besoin de rafraichir la liste */
    if (s_amount_panel && !lv_obj_has_flag(s_amount_panel, LV_OBJ_FLAG_HIDDEN)) {
        return;
    }

    const bool small = ctx->is_small_screen;

    if (xSemaphoreTake(ctx->state_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        uint32_t count = (ctx->peer_count) ? *ctx->peer_count : 0;

        /* Nettoyer la liste actuelle */
        lv_obj_clean(s_peer_list);

        if (count == 0) {
            /* Aucun peer : afficher le message */
            lv_obj_clear_flag(s_empty_label, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_peer_list, LV_OBJ_FLAG_HIDDEN);
        } else {
            /* Des peers existent : masquer le message, afficher la liste */
            lv_obj_add_flag(s_empty_label, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(s_peer_list, LV_OBJ_FLAG_HIDDEN);

            for (uint32_t i = 0; i < count; i++) {
                /* Carte pour chaque peer */
                lv_obj_t *card = lv_obj_create(s_peer_list);
                lv_obj_remove_style_all(card);
                lv_obj_add_style(card, &ui_style_card, 0);
                lv_obj_set_size(card, lv_pct(100), small ? 40 : 52);
                lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
                lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
                lv_obj_add_event_cb(card, peer_selected_cb, LV_EVENT_CLICKED,
                                    (void *)(uintptr_t)i);

                /* Alias du peer */
                lv_obj_t *alias_lbl = lv_label_create(card);
                lv_obj_set_style_text_font(alias_lbl, ui_theme_font_normal(), 0);
                lv_obj_set_style_text_color(alias_lbl, UI_COLOR_TEXT, 0);
                const char *alias = ctx->peers[i].alias;
                lv_label_set_text(alias_lbl,
                                  (alias[0] != '\0') ? alias : "Sans nom");
                lv_obj_align(alias_lbl, LV_ALIGN_LEFT_MID, small ? 6 : 10,
                             small ? 0 : -8);

                /* Cle publique tronquee (grand ecran : sous l'alias) */
                if (!small) {
                    char key_str[32];
                    format_key_short(key_str, sizeof(key_str),
                                     &ctx->peers[i].public_key);
                    lv_obj_t *key_lbl = lv_label_create(card);
                    lv_obj_set_style_text_font(key_lbl, ui_theme_font_normal(), 0);
                    lv_obj_set_style_text_color(key_lbl, UI_COLOR_TEXT_DIM, 0);
                    lv_label_set_text(key_lbl, key_str);
                    lv_obj_align(key_lbl, LV_ALIGN_LEFT_MID, 10, 8);
                }
            }
        }

        xSemaphoreGive(ctx->state_mutex);
    }
}

static void screen_destroy(void)
{
    if (s_screen) {
        lv_obj_delete(s_screen);
        s_screen        = NULL;
        s_peer_list     = NULL;
        s_empty_label   = NULL;
        s_amount_panel  = NULL;
        s_spinbox       = NULL;
        s_selected_label = NULL;
        s_ctx           = NULL;
        s_peer_selected = false;
    }
}

ui_screen_handler_t ui_screen_pay_handler = {
    .create  = screen_create,
    .update  = screen_update,
    .destroy = screen_destroy,
};
