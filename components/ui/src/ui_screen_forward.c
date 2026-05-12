/**
 * @file ui_screen_forward.c
 * @brief Ecran Configurer forward — Active/desactive l'auto-forward sur un device.
 *
 * Permet au maitre de configurer un device distant pour qu'il
 * forwarde automatiquement ses fonds vers un beneficiaire.
 *
 * Contenu :
 * - Header avec titre "Forward" et bouton retour
 * - Roller selecteur du device cible (ping_results)
 * - Roller selecteur du beneficiaire (ping_results)
 * - Roller selecteur d'intervalle (0=off, 5, 10, 30, 60 min)
 * - Bouton "Configurer" qui poste UI_CMD_SET_BENEFICIARY
 * - Feedback temporaire "Config envoyee !"
 *
 * Pas besoin de clavier virtuel → fonctionne sur tous les ecrans.
 */

#include "ui/ui_screens.h"
#include "ui/ui_state.h"
#include "ui/ui_theme.h"
#include "ui/ui_manager.h"

#include <string.h>
#include <stdio.h>

static lv_obj_t *s_screen          = NULL;
static lv_obj_t *s_roller_target   = NULL; /* Roller device cible */
static lv_obj_t *s_roller_benef    = NULL; /* Roller beneficiaire */
static lv_obj_t *s_roller_interval = NULL; /* Roller intervalle */
static lv_obj_t *s_lbl_feedback    = NULL; /* Label feedback */
static lv_obj_t *s_lbl_no_peer     = NULL; /* Label "aucun peer" */
static lv_obj_t *s_cont_form       = NULL; /* Conteneur du formulaire */
static lv_obj_t *s_btn_config      = NULL; /* Bouton configurer */

/** Dernier nombre de resultats connu */
static uint32_t s_last_result_count = 0;

/** Timer pour effacer le feedback */
static lv_timer_t *s_feedback_timer = NULL;

/** Table des intervalles disponibles (en minutes) */
static const uint16_t s_intervals[] = {0, 5, 10, 30, 60};
#define NUM_INTERVALS (sizeof(s_intervals) / sizeof(s_intervals[0]))

/* ----------------------------------------------------------------
 * Callbacks
 * ---------------------------------------------------------------- */

static void back_cb(lv_event_t *e)
{
    (void)e;
    ui_manager_back();
}

static void feedback_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    if (s_lbl_feedback) {
        lv_obj_add_flag(s_lbl_feedback, LV_OBJ_FLAG_HIDDEN);
    }
    s_feedback_timer = NULL;
}

/**
 * Callback du bouton "Configurer" : poste UI_CMD_SET_BENEFICIARY.
 *
 * Recupere le device cible, le beneficiaire et l'intervalle depuis
 * les rollers, puis envoie la commande dans la queue.
 */
static void config_cb(lv_event_t *e)
{
    ui_ctx_t *ctx = (ui_ctx_t *)lv_event_get_user_data(e);
    if (!ctx || !s_roller_target || !s_roller_benef || !s_roller_interval) {
        return;
    }

    uint32_t target_idx = lv_roller_get_selected(s_roller_target);
    uint32_t benef_idx  = lv_roller_get_selected(s_roller_benef);
    uint32_t interval_idx = lv_roller_get_selected(s_roller_interval);

    if (interval_idx >= NUM_INTERVALS) {
        return;
    }

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

    /* Recuperer les cles publiques des devices selectionnes */
    public_key_t target_key, benef_key;
    memset(&target_key, 0, sizeof(target_key));
    memset(&benef_key, 0, sizeof(benef_key));
    bool found_target = false;
    bool found_benef = false;

    if (ctx->ping_results && ctx->ping_result_count &&
        *ctx->ping_result_count > 0) {
        uint32_t count = *ctx->ping_result_count;
        if (target_idx < count) {
            memcpy(&target_key, &ctx->ping_results[target_idx].key,
                   sizeof(public_key_t));
            found_target = true;
        }
        if (benef_idx < count) {
            memcpy(&benef_key, &ctx->ping_results[benef_idx].key,
                   sizeof(public_key_t));
            found_benef = true;
        }
    } else if (ctx->peers && ctx->peer_count &&
               *ctx->peer_count > 0) {
        uint32_t count = *ctx->peer_count;
        if (target_idx < count) {
            memcpy(&target_key, &ctx->peers[target_idx].public_key,
                   sizeof(public_key_t));
            found_target = true;
        }
        if (benef_idx < count) {
            memcpy(&benef_key, &ctx->peers[benef_idx].public_key,
                   sizeof(public_key_t));
            found_benef = true;
        }
    }

    xSemaphoreGive(ctx->state_mutex);

    if (!found_target || !found_benef) {
        return;
    }

    /* Construire et poster la commande (la queue est thread-safe) */
    ui_cmd_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.type = UI_CMD_SET_BENEFICIARY;
    memcpy(&cmd.data.set_beneficiary.target, &target_key, sizeof(public_key_t));
    memcpy(&cmd.data.set_beneficiary.beneficiary, &benef_key, sizeof(public_key_t));
    cmd.data.set_beneficiary.interval_min = s_intervals[interval_idx];
    xQueueSend(ctx->cmd_queue, &cmd, pdMS_TO_TICKS(100));

    /* Afficher le feedback */
    if (s_lbl_feedback) {
        if (s_intervals[interval_idx] == 0) {
            lv_label_set_text(s_lbl_feedback, "Forward desactive !");
        } else {
            lv_label_set_text(s_lbl_feedback, "Config envoyee !");
        }
        lv_obj_clear_flag(s_lbl_feedback, LV_OBJ_FLAG_HIDDEN);

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
 * Construit la chaine d'options du roller des devices.
 * Meme logique que les ecrans mint et rename.
 *
 * Verifie que chaque entree tient integralement dans le buffer
 * avant de l'ecrire. Retourne le nombre d'entrees reellement ecrites.
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

    const bool small = ctx->is_small_screen;

    /* --- Header --- */
    lv_obj_t *header = lv_obj_create(s_screen);
    lv_obj_add_style(header, &ui_style_header, 0);
    lv_obj_set_size(header, lv_pct(100), small ? 32 : 44);
    lv_obj_align(header, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(header, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_hor(header, 8, 0);

    lv_obj_t *btn_back = lv_button_create(header);
    lv_obj_set_size(btn_back, small ? 40 : 50, small ? 24 : 32);
    lv_obj_add_style(btn_back, &ui_style_btn, 0);
    lv_obj_add_event_cb(btn_back, back_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *back_label = lv_label_create(btn_back);
    lv_label_set_text(back_label, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_color(back_label, UI_COLOR_TEXT, 0);
    lv_obj_center(back_label);

    lv_obj_t *title = lv_label_create(header);
    lv_obj_add_style(title, &ui_style_title, 0);
    lv_label_set_text(title, "Forward");
    lv_obj_set_style_pad_left(title, 8, 0);

    /* --- Message "aucun peer" --- */
    s_lbl_no_peer = lv_label_create(s_screen);
    lv_obj_add_style(s_lbl_no_peer, &ui_style_text, 0);
    lv_label_set_text(s_lbl_no_peer, "Lancez un scan d'abord");
    lv_obj_set_style_text_color(s_lbl_no_peer, UI_COLOR_WARNING, 0);
    lv_obj_align(s_lbl_no_peer, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(s_lbl_no_peer, LV_OBJ_FLAG_HIDDEN);

    /* --- Conteneur du formulaire --- */
    const int32_t header_h = small ? 34 : 48;

    s_cont_form = lv_obj_create(s_screen);
    lv_obj_set_style_bg_opa(s_cont_form, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_cont_form, 0, 0);
    lv_obj_set_size(s_cont_form, lv_pct(100),
                    ctx->screen_h - header_h);
    lv_obj_align(s_cont_form, LV_ALIGN_TOP_MID, 0, header_h);
    lv_obj_set_flex_flow(s_cont_form, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_cont_form, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(s_cont_form, small ? 4 : 10, 0);
    lv_obj_set_style_pad_row(s_cont_form, small ? 3 : 8, 0);
    lv_obj_add_flag(s_cont_form, LV_OBJ_FLAG_SCROLLABLE);

    /* Remplir les rollers avec les devices disponibles */
    char dest_buf[512];
    uint32_t dest_count = build_dest_options(ctx, dest_buf, sizeof(dest_buf));
    s_last_result_count = dest_count;

    if (dest_count == 0) {
        lv_obj_add_flag(s_cont_form, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_lbl_no_peer, LV_OBJ_FLAG_HIDDEN);
    }

    /* Label + roller "Device cible" */
    lv_obj_t *lbl_target = lv_label_create(s_cont_form);
    lv_obj_add_style(lbl_target, &ui_style_text, 0);
    lv_label_set_text(lbl_target, "Device cible :");

    s_roller_target = lv_roller_create(s_cont_form);
    lv_obj_set_width(s_roller_target, small ? lv_pct(90) : lv_pct(70));
    lv_roller_set_visible_row_count(s_roller_target, small ? 1 : 2);
    lv_obj_set_style_text_font(s_roller_target, ui_theme_font_normal(), 0);
    if (dest_count > 0) {
        lv_roller_set_options(s_roller_target, dest_buf, LV_ROLLER_MODE_NORMAL);
    }

    /* Label + roller "Beneficiaire" */
    lv_obj_t *lbl_benef = lv_label_create(s_cont_form);
    lv_obj_add_style(lbl_benef, &ui_style_text, 0);
    lv_label_set_text(lbl_benef, "Beneficiaire :");

    s_roller_benef = lv_roller_create(s_cont_form);
    lv_obj_set_width(s_roller_benef, small ? lv_pct(90) : lv_pct(70));
    lv_roller_set_visible_row_count(s_roller_benef, small ? 1 : 2);
    lv_obj_set_style_text_font(s_roller_benef, ui_theme_font_normal(), 0);
    if (dest_count > 0) {
        lv_roller_set_options(s_roller_benef, dest_buf, LV_ROLLER_MODE_NORMAL);
    }

    /* Label + roller "Intervalle" */
    lv_obj_t *lbl_interval = lv_label_create(s_cont_form);
    lv_obj_add_style(lbl_interval, &ui_style_text, 0);
    lv_label_set_text(lbl_interval, "Intervalle :");

    s_roller_interval = lv_roller_create(s_cont_form);
    lv_obj_set_width(s_roller_interval, small ? lv_pct(60) : lv_pct(50));
    lv_roller_set_visible_row_count(s_roller_interval, small ? 1 : 2);
    lv_obj_set_style_text_font(s_roller_interval, ui_theme_font_normal(), 0);
    lv_roller_set_options(s_roller_interval,
                          "Desactive\n5 min\n10 min\n30 min\n60 min",
                          LV_ROLLER_MODE_NORMAL);

    /* Bouton "Configurer" */
    s_btn_config = lv_button_create(s_cont_form);
    lv_obj_add_style(s_btn_config, &ui_style_btn, 0);
    lv_obj_set_size(s_btn_config, small ? lv_pct(70) : lv_pct(50),
                    small ? 34 : 44);
    lv_obj_set_style_bg_color(s_btn_config, UI_COLOR_ACCENT, 0);
    lv_obj_add_event_cb(s_btn_config, config_cb, LV_EVENT_CLICKED, ctx);

    lv_obj_t *btn_label = lv_label_create(s_btn_config);
    lv_obj_set_style_text_font(btn_label, ui_theme_font_normal(), 0);
    lv_obj_set_style_text_color(btn_label, UI_COLOR_TEXT, 0);
    lv_label_set_text(btn_label, "Configurer");
    lv_obj_center(btn_label);

    /* Label feedback (masque) */
    s_lbl_feedback = lv_label_create(s_cont_form);
    lv_obj_set_style_text_font(s_lbl_feedback, ui_theme_font_normal(), 0);
    lv_obj_set_style_text_color(s_lbl_feedback, UI_COLOR_SUCCESS, 0);
    lv_label_set_text(s_lbl_feedback, "Config envoyee !");
    lv_obj_add_flag(s_lbl_feedback, LV_OBJ_FLAG_HIDDEN);

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
            lv_roller_set_options(s_roller_target, dest_buf,
                                  LV_ROLLER_MODE_NORMAL);
            lv_roller_set_options(s_roller_benef, dest_buf,
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

    s_roller_target     = NULL;
    s_roller_benef      = NULL;
    s_roller_interval   = NULL;
    s_lbl_no_peer       = NULL;
    s_cont_form         = NULL;
    s_btn_config        = NULL;
    s_last_result_count = 0;
}

ui_screen_handler_t ui_screen_forward_handler = {
    .create  = screen_create,
    .update  = screen_update,
    .destroy = screen_destroy,
};
