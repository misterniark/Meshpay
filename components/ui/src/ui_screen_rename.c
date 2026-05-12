/**
 * @file ui_screen_rename.c
 * @brief Ecran Renommer device — Attribution d'un alias a un device distant.
 *
 * Permet au maitre de choisir un device cible parmi les resultats
 * de ping et de lui attribuer un nouvel alias via UI_CMD_SET_ALIAS.
 *
 * Contenu :
 * - Header avec titre "Renommer" et bouton retour
 * - Roller selecteur de device cible (ping_results ou peers)
 * - Zone de saisie du nouvel alias (textarea, max 32 caracteres)
 * - Bouton "Renommer" qui poste UI_CMD_SET_ALIAS
 * - Clavier virtuel attache a la textarea
 * - Feedback "Alias envoye !" temporaire
 *
 * Note : cet ecran n'est accessible que sur grand ecran (CYD 320x240)
 * car le clavier virtuel necessite suffisamment de hauteur.
 */

#include "ui/ui_screens.h"
#include "ui/ui_state.h"
#include "ui/ui_theme.h"
#include "ui/ui_manager.h"

#include "comm/comm_msg.h"

#include <string.h>
#include <stdio.h>

static lv_obj_t *s_screen       = NULL;
static lv_obj_t *s_roller_dest  = NULL; /* Roller selecteur de device */
static lv_obj_t *s_textarea     = NULL; /* Zone de saisie alias */
static lv_obj_t *s_btn_rename   = NULL; /* Bouton renommer */
static lv_obj_t *s_keyboard     = NULL; /* Clavier virtuel */
static lv_obj_t *s_lbl_feedback = NULL; /* Label feedback */
static lv_obj_t *s_lbl_no_peer  = NULL; /* Label "aucun peer" */
static lv_obj_t *s_cont_form    = NULL; /* Conteneur du formulaire */

/** Dernier nombre de resultats connu, pour detecter les changements */
static uint32_t s_last_result_count = 0;

/** Timer LVGL pour effacer le feedback apres un delai */
static lv_timer_t *s_feedback_timer = NULL;

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
 * Callback du timer de feedback : masque le label apres 2 secondes.
 */
static void feedback_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    if (s_lbl_feedback) {
        lv_obj_add_flag(s_lbl_feedback, LV_OBJ_FLAG_HIDDEN);
    }
    s_feedback_timer = NULL;
}

/**
 * Callback du bouton "Renommer" : envoie la commande UI_CMD_SET_ALIAS.
 *
 * Recupere le device cible selectionne et le nouvel alias, puis
 * poste la commande dans la queue.
 */
static void rename_cb(lv_event_t *e)
{
    ui_ctx_t *ctx = (ui_ctx_t *)lv_event_get_user_data(e);
    if (!ctx || !s_roller_dest || !s_textarea) {
        return;
    }

    /* Lire le nouvel alias depuis la textarea */
    const char *text = lv_textarea_get_text(s_textarea);
    uint32_t len = (uint32_t)strlen(text);
    if (len == 0 || len > COMM_MSG_ALIAS_MAX) {
        return;
    }

    /* Recuperer l'index du device cible selectionne */
    uint32_t dest_idx = lv_roller_get_selected(s_roller_dest);

    /* Proteger la lecture des donnees partagees par le mutex */
    if (xSemaphoreTake(ctx->state_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        /* Mutex indisponible : afficher une erreur et abandonner */
        if (s_lbl_feedback) {
            lv_label_set_text(s_lbl_feedback, "Erreur : ressource occupee");
            lv_obj_set_style_text_color(s_lbl_feedback, UI_COLOR_WARNING, 0);
            lv_obj_clear_flag(s_lbl_feedback, LV_OBJ_FLAG_HIDDEN);
        }
        return;
    }

    /* Determiner la cle publique du device cible.
     * Priorite aux ping_results (plus recents), sinon les peers. */
    public_key_t target_key;
    memset(&target_key, 0, sizeof(target_key));
    bool found = false;

    if (ctx->ping_results && ctx->ping_result_count &&
        *ctx->ping_result_count > 0) {
        if (dest_idx < *ctx->ping_result_count) {
            memcpy(&target_key, &ctx->ping_results[dest_idx].key,
                   sizeof(public_key_t));
            found = true;
        }
    } else if (ctx->peers && ctx->peer_count &&
               *ctx->peer_count > 0) {
        if (dest_idx < *ctx->peer_count) {
            memcpy(&target_key, &ctx->peers[dest_idx].public_key,
                   sizeof(public_key_t));
            found = true;
        }
    }

    xSemaphoreGive(ctx->state_mutex);

    if (!found) {
        return;
    }

    /* Construire et poster la commande (la queue est thread-safe) */
    ui_cmd_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.type = UI_CMD_SET_ALIAS;
    memcpy(&cmd.data.set_alias.target, &target_key, sizeof(public_key_t));
    memcpy(cmd.data.set_alias.alias, text, len);
    cmd.data.set_alias.alias[len] = '\0';
    cmd.data.set_alias.alias_len = (uint8_t)len;
    xQueueSend(ctx->cmd_queue, &cmd, pdMS_TO_TICKS(100));

    /* Afficher le feedback */
    if (s_lbl_feedback) {
        lv_label_set_text(s_lbl_feedback, "Alias envoye !");
        lv_obj_clear_flag(s_lbl_feedback, LV_OBJ_FLAG_HIDDEN);

        if (s_feedback_timer) {
            lv_timer_delete(s_feedback_timer);
        }
        s_feedback_timer = lv_timer_create(feedback_timer_cb, 2000, NULL);
        lv_timer_set_repeat_count(s_feedback_timer, 1);
    }

    /* Vider la textarea apres envoi */
    lv_textarea_set_text(s_textarea, "");
}

/**
 * Callback de changement de texte : active/desactive le bouton Renommer
 * selon que le texte est vide ou non.
 */
static void textarea_changed_cb(lv_event_t *e)
{
    (void)e;
    if (!s_textarea || !s_btn_rename) {
        return;
    }

    const char *text = lv_textarea_get_text(s_textarea);
    uint32_t len = (uint32_t)strlen(text);

    if (len == 0 || len > COMM_MSG_ALIAS_MAX) {
        lv_obj_add_state(s_btn_rename, LV_STATE_DISABLED);
        lv_obj_set_style_opa(s_btn_rename, LV_OPA_50, LV_STATE_DISABLED);
    } else {
        lv_obj_clear_state(s_btn_rename, LV_STATE_DISABLED);
        lv_obj_set_style_opa(s_btn_rename, LV_OPA_COVER, 0);
    }
}

/* ----------------------------------------------------------------
 * Utilitaires
 * ---------------------------------------------------------------- */

/**
 * Construit la chaine d'options du roller des devices cibles.
 * Meme logique que l'ecran mint : priorite aux ping_results.
 *
 * Verifie que chaque entree tient integralement dans le buffer
 * avant de l'ecrire. Retourne le nombre d'entrees reellement ecrites.
 *
 * @param ctx  Contexte UI
 * @param buf  Buffer de sortie
 * @param size Taille du buffer
 * @return     Nombre de devices reellement ecrits dans le buffer
 */
static uint32_t build_dest_options(ui_ctx_t *ctx, char *buf, size_t size)
{
    buf[0] = '\0';
    uint32_t written_count = 0;
    size_t offset = 0;

    if (ctx->ping_results && ctx->ping_result_count &&
        *ctx->ping_result_count > 0) {
        uint32_t total = *ctx->ping_result_count;
        for (uint32_t i = 0; i < total; i++) {
            size_t remaining = size - offset;
            if (remaining < 42) {
                break; /* Plus assez de place pour une entree complete */
            }
            int written = snprintf(buf + offset, remaining,
                                   "%s%.*s (%02X%02X)",
                                   (written_count > 0) ? "\n" : "",
                                   ctx->ping_results[i].alias_len,
                                   ctx->ping_results[i].alias,
                                   ctx->ping_results[i].key.bytes[0],
                                   ctx->ping_results[i].key.bytes[1]);
            if (written < 0 || (size_t)written >= remaining) {
                buf[offset] = '\0';
                break;
            }
            offset += (size_t)written;
            written_count++;
        }
    } else if (ctx->peers && ctx->peer_count &&
               *ctx->peer_count > 0) {
        uint32_t total = *ctx->peer_count;
        for (uint32_t i = 0; i < total; i++) {
            size_t remaining = size - offset;
            if (remaining < 42) {
                break;
            }
            int written = snprintf(buf + offset, remaining,
                                   "%s%s (%02X%02X)",
                                   (written_count > 0) ? "\n" : "",
                                   ctx->peers[i].alias,
                                   ctx->peers[i].public_key.bytes[0],
                                   ctx->peers[i].public_key.bytes[1]);
            if (written < 0 || (size_t)written >= remaining) {
                buf[offset] = '\0';
                break;
            }
            offset += (size_t)written;
            written_count++;
        }
    }

    return written_count;
}

/* ----------------------------------------------------------------
 * Interface ecran
 * ---------------------------------------------------------------- */

static lv_obj_t *screen_create(ui_ctx_t *ctx)
{
    s_screen = lv_obj_create(NULL);
    lv_obj_add_style(s_screen, &ui_style_screen, 0);

    /* Cet ecran n'est affiche que sur grand ecran (CYD 320x240).
     * On utilise donc directement les tailles grand ecran. */

    /* --- Header : bouton retour + titre --- */
    lv_obj_t *header = lv_obj_create(s_screen);
    lv_obj_add_style(header, &ui_style_header, 0);
    lv_obj_set_size(header, lv_pct(100), 44);
    lv_obj_align(header, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(header, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_hor(header, 8, 0);

    lv_obj_t *btn_back = lv_button_create(header);
    lv_obj_set_size(btn_back, 50, 32);
    lv_obj_add_style(btn_back, &ui_style_btn, 0);
    lv_obj_add_event_cb(btn_back, back_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *back_label = lv_label_create(btn_back);
    lv_label_set_text(back_label, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_color(back_label, UI_COLOR_TEXT, 0);
    lv_obj_center(back_label);

    lv_obj_t *title = lv_label_create(header);
    lv_obj_add_style(title, &ui_style_title, 0);
    lv_label_set_text(title, "Renommer");
    lv_obj_set_style_pad_left(title, 8, 0);

    /* --- Message "aucun peer" (masque par defaut) --- */
    s_lbl_no_peer = lv_label_create(s_screen);
    lv_obj_add_style(s_lbl_no_peer, &ui_style_text, 0);
    lv_label_set_text(s_lbl_no_peer, "Lancez un scan d'abord");
    lv_obj_set_style_text_color(s_lbl_no_peer, UI_COLOR_WARNING, 0);
    lv_obj_align(s_lbl_no_peer, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(s_lbl_no_peer, LV_OBJ_FLAG_HIDDEN);

    /* --- Conteneur du formulaire (au-dessus du clavier) --- */
    const int32_t header_h = 48;
    const int32_t kb_h     = 140;

    s_cont_form = lv_obj_create(s_screen);
    lv_obj_set_style_bg_opa(s_cont_form, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_cont_form, 0, 0);
    lv_obj_set_size(s_cont_form, lv_pct(100),
                    ctx->screen_h - header_h - kb_h);
    lv_obj_align(s_cont_form, LV_ALIGN_TOP_MID, 0, header_h);
    lv_obj_set_flex_flow(s_cont_form, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(s_cont_form, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_all(s_cont_form, 6, 0);
    lv_obj_set_style_pad_row(s_cont_form, 4, 0);
    lv_obj_set_style_pad_column(s_cont_form, 8, 0);
    lv_obj_clear_flag(s_cont_form, LV_OBJ_FLAG_SCROLLABLE);

    /* Label "Device :" */
    lv_obj_t *lbl_dest = lv_label_create(s_cont_form);
    lv_obj_add_style(lbl_dest, &ui_style_text, 0);
    lv_label_set_text(lbl_dest, "Device :");
    lv_obj_set_width(lbl_dest, lv_pct(100));

    /* Roller des devices cibles */
    s_roller_dest = lv_roller_create(s_cont_form);
    lv_obj_set_width(s_roller_dest, lv_pct(55));
    lv_roller_set_visible_row_count(s_roller_dest, 1);
    lv_obj_set_style_text_font(s_roller_dest, ui_theme_font_normal(), 0);

    /* Remplir le roller avec les devices disponibles */
    char dest_buf[512];
    uint32_t dest_count = build_dest_options(ctx, dest_buf, sizeof(dest_buf));
    s_last_result_count = dest_count;

    if (dest_count > 0) {
        lv_roller_set_options(s_roller_dest, dest_buf, LV_ROLLER_MODE_NORMAL);
    } else {
        lv_obj_add_flag(s_cont_form, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_lbl_no_peer, LV_OBJ_FLAG_HIDDEN);
    }

    /* Textarea pour le nouvel alias */
    s_textarea = lv_textarea_create(s_cont_form);
    lv_obj_set_size(s_textarea, lv_pct(38), 30);
    lv_textarea_set_max_length(s_textarea, COMM_MSG_ALIAS_MAX);
    lv_textarea_set_placeholder_text(s_textarea, "Nouvel alias");
    lv_textarea_set_one_line(s_textarea, true);
    lv_obj_set_style_text_font(s_textarea, ui_theme_font_normal(), 0);
    lv_obj_set_style_bg_color(s_textarea, UI_COLOR_CARD, 0);
    lv_obj_set_style_text_color(s_textarea, UI_COLOR_TEXT, 0);
    lv_obj_set_style_border_color(s_textarea, UI_COLOR_ACCENT, LV_STATE_FOCUSED);
    lv_obj_add_event_cb(s_textarea, textarea_changed_cb,
                        LV_EVENT_VALUE_CHANGED, NULL);

    /* Bouton "Renommer" */
    s_btn_rename = lv_button_create(s_cont_form);
    lv_obj_add_style(s_btn_rename, &ui_style_btn, 0);
    lv_obj_set_size(s_btn_rename, lv_pct(30), 30);
    lv_obj_set_style_bg_color(s_btn_rename, UI_COLOR_ACCENT, 0);
    lv_obj_add_event_cb(s_btn_rename, rename_cb, LV_EVENT_CLICKED, ctx);

    /* Desactive par defaut (textarea vide) */
    lv_obj_add_state(s_btn_rename, LV_STATE_DISABLED);
    lv_obj_set_style_opa(s_btn_rename, LV_OPA_50, LV_STATE_DISABLED);

    lv_obj_t *btn_label = lv_label_create(s_btn_rename);
    lv_obj_set_style_text_font(btn_label, ui_theme_font_normal(), 0);
    lv_obj_set_style_text_color(btn_label, UI_COLOR_TEXT, 0);
    lv_label_set_text(btn_label, "Renommer");
    lv_obj_center(btn_label);

    /* Label de feedback (masque par defaut) */
    s_lbl_feedback = lv_label_create(s_cont_form);
    lv_obj_set_style_text_font(s_lbl_feedback, ui_theme_font_normal(), 0);
    lv_obj_set_style_text_color(s_lbl_feedback, UI_COLOR_SUCCESS, 0);
    lv_label_set_text(s_lbl_feedback, "Alias envoye !");
    lv_obj_add_flag(s_lbl_feedback, LV_OBJ_FLAG_HIDDEN);

    /* --- Clavier virtuel --- */
    s_keyboard = lv_keyboard_create(s_screen);
    lv_obj_set_size(s_keyboard, lv_pct(100), kb_h);
    lv_obj_align(s_keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_textarea(s_keyboard, s_textarea);

    return s_screen;
}

static void screen_update(ui_ctx_t *ctx)
{
    if (!s_screen || !ctx) {
        return;
    }

    if (xSemaphoreTake(ctx->state_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return;
    }

    /* Verifier si le nombre de devices cibles a change */
    uint32_t current_count = 0;
    if (ctx->ping_results && ctx->ping_result_count) {
        current_count = *ctx->ping_result_count;
    } else if (ctx->peers && ctx->peer_count) {
        current_count = *ctx->peer_count;
    }

    if (current_count != s_last_result_count) {
        s_last_result_count = current_count;

        if (current_count > 0) {
            char dest_buf[512];
            build_dest_options(ctx, dest_buf, sizeof(dest_buf));
            lv_roller_set_options(s_roller_dest, dest_buf,
                                  LV_ROLLER_MODE_NORMAL);
            lv_obj_clear_flag(s_cont_form, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_lbl_no_peer, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_cont_form, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(s_lbl_no_peer, LV_OBJ_FLAG_HIDDEN);
        }
    }

    xSemaphoreGive(ctx->state_mutex);
}

static void screen_destroy(void)
{
    /* Mettre le label a NULL AVANT de supprimer le timer
     * pour eviter un use-after-free si le callback s'execute
     * entre la suppression de l'ecran et celle du timer. */
    s_lbl_feedback = NULL;

    if (s_feedback_timer) {
        lv_timer_delete(s_feedback_timer);
        s_feedback_timer = NULL;
    }

    if (s_screen) {
        lv_obj_delete(s_screen);
        s_screen = NULL;
    }

    s_roller_dest       = NULL;
    s_textarea          = NULL;
    s_btn_rename        = NULL;
    s_keyboard          = NULL;
    s_lbl_no_peer       = NULL;
    s_cont_form         = NULL;
    s_last_result_count = 0;
}

ui_screen_handler_t ui_screen_rename_handler = {
    .create  = screen_create,
    .update  = screen_update,
    .destroy = screen_destroy,
};
