/**
 * @file ui_screen_mint.c
 * @brief Ecran Creer des credits — Interface de creation (mint) de tokens.
 *
 * Permet au maitre de creer des credits pour un destinataire choisi
 * parmi les resultats de ping ou les peers connus.
 *
 * Contenu :
 * - Header avec titre et bouton retour
 * - Selecteur de destinataire (roller des ping_results)
 * - Selecteur de montant (roller 100..10000 par pas de 100)
 * - Bouton "Creer" qui poste UI_CMD_MINT
 * - Feedback temporaire "Credits crees !"
 * - Message si aucun peer : "Lancez un scan d'abord"
 */

#include "ui/ui_screens.h"
#include "ui/ui_state.h"
#include "ui/ui_theme.h"
#include "ui/ui_manager.h"

#include <string.h>
#include <stdio.h>

static lv_obj_t *s_screen = NULL;

/* References vers les widgets dynamiques (mis a jour dans update) */
static lv_obj_t *s_roller_dest   = NULL; /* Roller selecteur de destinataire */
static lv_obj_t *s_roller_amount = NULL; /* Roller selecteur de montant */
static lv_obj_t *s_lbl_feedback  = NULL; /* Label de feedback */
static lv_obj_t *s_lbl_no_peer   = NULL; /* Label "aucun peer" */
static lv_obj_t *s_btn_create    = NULL; /* Bouton creer */
static lv_obj_t *s_cont_form     = NULL; /* Conteneur du formulaire */

/* Dernier nombre de resultats connu, pour detecter les changements */
static uint32_t s_last_result_count = 0;

/* Timer LVGL pour effacer le feedback apres un delai */
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
 * Callback du bouton "Creer" : envoie la commande UI_CMD_MINT.
 *
 * Recupere le destinataire selectionne et le montant, puis poste
 * la commande dans la queue sans prendre le mutex.
 */
static void create_cb(lv_event_t *e)
{
    ui_ctx_t *ctx = (ui_ctx_t *)lv_event_get_user_data(e);
    if (!ctx || !s_roller_dest || !s_roller_amount) {
        return;
    }

    /* Recuperer l'index du destinataire selectionne */
    uint32_t dest_idx = lv_roller_get_selected(s_roller_dest);

    /* Recuperer le montant : options de 100 a 10000 par pas de 100 */
    uint32_t amount_idx = lv_roller_get_selected(s_roller_amount);
    uint32_t amount = (amount_idx + 1) * 100; /* 0→100, 1→200, ..., 99→10000 */

    /* Proteger la lecture des donnees partagees (peers/ping_results) par le mutex */
    if (xSemaphoreTake(ctx->state_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        /* Mutex indisponible : afficher une erreur et abandonner */
        if (s_lbl_feedback) {
            lv_label_set_text(s_lbl_feedback, "Erreur : ressource occupee");
            lv_obj_set_style_text_color(s_lbl_feedback, UI_COLOR_WARNING, 0);
            lv_obj_clear_flag(s_lbl_feedback, LV_OBJ_FLAG_HIDDEN);
        }
        return;
    }

    /* Determiner la cle publique du destinataire */
    /* On utilise les ping_results en priorite, sinon les peers */
    public_key_t dest_key;
    memset(&dest_key, 0, sizeof(dest_key));

    if (ctx->ping_results && *ctx->ping_result_count > 0) {
        if (dest_idx < *ctx->ping_result_count) {
            memcpy(&dest_key, &ctx->ping_results[dest_idx].key, sizeof(public_key_t));
        }
    } else if (ctx->peers && *ctx->peer_count > 0) {
        if (dest_idx < *ctx->peer_count) {
            memcpy(&dest_key, &ctx->peers[dest_idx].public_key, sizeof(public_key_t));
        }
    }

    xSemaphoreGive(ctx->state_mutex);

    /* Construire et poster la commande (sans mutex, la queue est thread-safe) */
    ui_cmd_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.type = UI_CMD_MINT;
    memcpy(&cmd.data.mint.to, &dest_key, sizeof(public_key_t));
    cmd.data.mint.amount = amount;
    xQueueSend(ctx->cmd_queue, &cmd, pdMS_TO_TICKS(100));

    /* Afficher le feedback */
    if (s_lbl_feedback) {
        lv_label_set_text(s_lbl_feedback, "Credits crees !");
        lv_obj_clear_flag(s_lbl_feedback, LV_OBJ_FLAG_HIDDEN);

        /* Demarrer un timer pour masquer le feedback apres 2s */
        if (s_feedback_timer) {
            lv_timer_delete(s_feedback_timer);
        }
        s_feedback_timer = lv_timer_create(feedback_timer_cb, 2000, NULL);
        lv_timer_set_repeat_count(s_feedback_timer, 1);
    }
}

/* ----------------------------------------------------------------
 * Utilitaires
 * ---------------------------------------------------------------- */

/**
 * Construit la chaine d'options du roller des destinataires.
 *
 * Format : "alias (XXXX...XXXX)" pour chaque resultat de ping
 * ou peer connu. Les options sont separees par '\n'.
 *
 * Verifie que chaque entree entiere tient dans le buffer AVANT
 * de l'ecrire. Retourne le nombre d'entrees reellement ecrites
 * (peut etre inferieur au nombre total si le buffer est plein).
 *
 * @param ctx  Contexte UI
 * @param buf  Buffer de sortie
 * @param size Taille du buffer
 * @return     Nombre de destinataires reellement ecrits dans le buffer
 */
static uint32_t build_dest_options(ui_ctx_t *ctx, char *buf, size_t size)
{
    buf[0] = '\0';
    uint32_t written_count = 0;
    size_t offset = 0;

    /* Priorite aux resultats de ping (plus recents) */
    if (ctx->ping_results && *ctx->ping_result_count > 0) {
        uint32_t total = *ctx->ping_result_count;
        for (uint32_t i = 0; i < total; i++) {
            /* Verifier qu'il reste assez de place pour une entree complete.
             * Taille max d'une entree : separateur(1) + alias(32) + " ("(2) + hex(4) + ")"(1) + null(1) = ~41 */
            size_t remaining = size - offset;
            if (remaining < 42) {
                break; /* Plus assez de place, on arrete */
            }
            int written = snprintf(buf + offset, remaining,
                                   "%s%.*s (%02X%02X)",
                                   (written_count > 0) ? "\n" : "",
                                   ctx->ping_results[i].alias_len,
                                   ctx->ping_results[i].alias,
                                   ctx->ping_results[i].key.bytes[0],
                                   ctx->ping_results[i].key.bytes[1]);
            /* Verifier que snprintf n'a pas tronque l'entree */
            if (written < 0 || (size_t)written >= remaining) {
                buf[offset] = '\0'; /* Annuler l'entree tronquee */
                break;
            }
            offset += (size_t)written;
            written_count++;
        }
    } else if (ctx->peers && *ctx->peer_count > 0) {
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
    lv_label_set_text(title, "Creer credits");
    lv_obj_set_style_pad_left(title, 8, 0);

    /* --- Message "aucun peer" (masque par defaut) --- */
    s_lbl_no_peer = lv_label_create(s_screen);
    lv_obj_add_style(s_lbl_no_peer, &ui_style_text, 0);
    lv_label_set_text(s_lbl_no_peer, "Lancez un scan d'abord");
    lv_obj_set_style_text_color(s_lbl_no_peer, UI_COLOR_WARNING, 0);
    lv_obj_align(s_lbl_no_peer, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(s_lbl_no_peer, LV_OBJ_FLAG_HIDDEN);

    /* --- Conteneur du formulaire --- */
    s_cont_form = lv_obj_create(s_screen);
    lv_obj_set_style_bg_opa(s_cont_form, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_cont_form, 0, 0);
    lv_obj_set_size(s_cont_form, lv_pct(100),
                    small ? (ctx->screen_h - 40) : (ctx->screen_h - 54));
    lv_obj_align(s_cont_form, LV_ALIGN_TOP_MID, 0, small ? 38 : 48);
    lv_obj_set_flex_flow(s_cont_form, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_cont_form, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(s_cont_form, small ? 4 : 12, 0);
    lv_obj_set_style_pad_row(s_cont_form, small ? 4 : 12, 0);

    /* Label "Destinataire" */
    lv_obj_t *lbl_dest = lv_label_create(s_cont_form);
    lv_obj_add_style(lbl_dest, &ui_style_text, 0);
    lv_label_set_text(lbl_dest, "Destinataire :");

    /* Roller des destinataires */
    s_roller_dest = lv_roller_create(s_cont_form);
    lv_obj_set_width(s_roller_dest, small ? lv_pct(90) : lv_pct(80));
    lv_roller_set_visible_row_count(s_roller_dest, small ? 1 : 3);
    lv_obj_set_style_text_font(s_roller_dest, ui_theme_font_normal(), 0);

    /* Remplir le roller avec les destinataires disponibles */
    char dest_buf[512];
    uint32_t dest_count = build_dest_options(ctx, dest_buf, sizeof(dest_buf));
    s_last_result_count = dest_count;

    if (dest_count > 0) {
        lv_roller_set_options(s_roller_dest, dest_buf, LV_ROLLER_MODE_NORMAL);
    } else {
        /* Pas de destinataire : masquer le formulaire, afficher le message */
        lv_obj_add_flag(s_cont_form, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_lbl_no_peer, LV_OBJ_FLAG_HIDDEN);
    }

    /* Label "Montant" */
    lv_obj_t *lbl_amount = lv_label_create(s_cont_form);
    lv_obj_add_style(lbl_amount, &ui_style_text, 0);
    lv_label_set_text(lbl_amount, "Montant :");

    /* Roller du montant : 100, 200, ..., 10000 */
    s_roller_amount = lv_roller_create(s_cont_form);
    lv_obj_set_width(s_roller_amount, small ? lv_pct(60) : lv_pct(50));
    lv_roller_set_visible_row_count(s_roller_amount, small ? 1 : 3);
    lv_obj_set_style_text_font(s_roller_amount, ui_theme_font_normal(), 0);

    /* Generer les options du montant */
    char amount_buf[1024];
    size_t off = 0;
    for (int v = 100; v <= 10000; v += 100) {
        int w = snprintf(amount_buf + off, sizeof(amount_buf) - off,
                         "%s%d", (v > 100) ? "\n" : "", v);
        if (w > 0) {
            off += (size_t)w;
        }
    }
    lv_roller_set_options(s_roller_amount, amount_buf, LV_ROLLER_MODE_NORMAL);

    /* Bouton "Creer" */
    s_btn_create = lv_button_create(s_cont_form);
    lv_obj_add_style(s_btn_create, &ui_style_btn, 0);
    lv_obj_set_size(s_btn_create, small ? lv_pct(70) : lv_pct(50),
                    small ? 36 : 44);
    lv_obj_set_style_bg_color(s_btn_create, UI_COLOR_SUCCESS, 0);
    lv_obj_add_event_cb(s_btn_create, create_cb, LV_EVENT_CLICKED, ctx);

    lv_obj_t *btn_label = lv_label_create(s_btn_create);
    lv_obj_set_style_text_font(btn_label, ui_theme_font_normal(), 0);
    lv_obj_set_style_text_color(btn_label, UI_COLOR_TEXT, 0);
    lv_label_set_text(btn_label, "Creer");
    lv_obj_center(btn_label);

    /* Label de feedback (masque par defaut) */
    s_lbl_feedback = lv_label_create(s_cont_form);
    lv_obj_set_style_text_font(s_lbl_feedback, ui_theme_font_normal(), 0);
    lv_obj_set_style_text_color(s_lbl_feedback, UI_COLOR_SUCCESS, 0);
    lv_label_set_text(s_lbl_feedback, "Credits crees !");
    lv_obj_add_flag(s_lbl_feedback, LV_OBJ_FLAG_HIDDEN);

    return s_screen;
}

static void screen_update(ui_ctx_t *ctx)
{
    if (!s_screen || !ctx) {
        return;
    }

    /* Prendre le mutex pour lire les donnees partagees */
    if (xSemaphoreTake(ctx->state_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return;
    }

    /* Verifier si le nombre de destinataires a change */
    uint32_t current_count = 0;
    if (ctx->ping_results && ctx->ping_result_count) {
        current_count = *ctx->ping_result_count;
    } else if (ctx->peers && ctx->peer_count) {
        current_count = *ctx->peer_count;
    }

    if (current_count != s_last_result_count) {
        s_last_result_count = current_count;

        if (current_count > 0) {
            /* Mettre a jour le roller des destinataires */
            char dest_buf[512];
            build_dest_options(ctx, dest_buf, sizeof(dest_buf));
            lv_roller_set_options(s_roller_dest, dest_buf, LV_ROLLER_MODE_NORMAL);

            /* Afficher le formulaire, masquer le message "aucun peer" */
            lv_obj_clear_flag(s_cont_form, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_lbl_no_peer, LV_OBJ_FLAG_HIDDEN);
        } else {
            /* Masquer le formulaire, afficher le message */
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

    /* Reinitialiser les references statiques */
    s_roller_dest    = NULL;
    s_roller_amount  = NULL;
    s_lbl_no_peer    = NULL;
    s_btn_create     = NULL;
    s_cont_form      = NULL;
    s_last_result_count = 0;
}

ui_screen_handler_t ui_screen_mint_handler = {
    .create  = screen_create,
    .update  = screen_update,
    .destroy = screen_destroy,
};
