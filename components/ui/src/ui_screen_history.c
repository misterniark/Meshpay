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

#include "esp_heap_caps.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Nombre maximum de TX affichees dans la liste */
#define HISTORY_MAX_DISPLAY 50
#define HISTORY_MAX_SOURCE  128

/* ================================================================
 * Variables statiques de l'ecran
 * ================================================================ */

static lv_obj_t *s_screen     = NULL;
/** Liste scrollable des transactions */
static lv_obj_t *s_tx_list    = NULL;
/** Label affiche quand le DAG est vide */
static lv_obj_t *s_empty_label = NULL;
static lv_obj_t *s_detail_modal = NULL;

static transaction_t *s_history_cache = NULL;
static transaction_t *s_history_source = NULL;
static uint32_t s_history_count = 0;
static ui_ctx_t *s_active_ctx = NULL;

/** Dernier nombre de TX connu, pour eviter de reconstruire inutilement */
static uint32_t s_last_count = UINT32_MAX;
static uint32_t s_last_persisted_count = UINT32_MAX;
static uint32_t s_last_refresh_tick = 0;

/* ================================================================
 * Callbacks
 * ================================================================ */

/** Retour a l'ecran precedent */
static void back_cb(lv_event_t *e)
{
    (void)e;
    ui_manager_back();
}

static void detail_close_cb(lv_event_t *e)
{
    (void)e;
    if (s_detail_modal) {
        lv_obj_delete(s_detail_modal);
        s_detail_modal = NULL;
    }
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

static bool history_buffers_ready(void)
{
    if (s_history_cache == NULL) {
        s_history_cache = heap_caps_malloc(sizeof(transaction_t) * HISTORY_MAX_DISPLAY,
                                           MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (s_history_cache == NULL) {
            s_history_cache = malloc(sizeof(transaction_t) * HISTORY_MAX_DISPLAY);
        }
    }
    if (s_history_source == NULL) {
        s_history_source = heap_caps_malloc(sizeof(transaction_t) * HISTORY_MAX_SOURCE,
                                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (s_history_source == NULL) {
            s_history_source = malloc(sizeof(transaction_t) * HISTORY_MAX_SOURCE);
        }
    }

    return s_history_cache != NULL && s_history_source != NULL;
}

static void format_tx_datetime(char *buf, size_t buf_len, uint64_t ts_ms)
{
    /*
     * Les tests actuels utilisent parfois un temps Lamport tres petit
     * (ex. timestamp=1). Dans ce cas on affiche explicitement la valeur
     * technique au lieu de fabriquer une fausse date Unix.
     */
    if (ts_ms < 1577836800000ULL) {
        snprintf(buf, buf_len, "t=%"PRIu64" ms", ts_ms);
        return;
    }

    time_t sec = (time_t)(ts_ms / 1000ULL);
    struct tm tm_info;
    localtime_r(&sec, &tm_info);
    snprintf(buf, buf_len, "%04d-%02d-%02d %02d:%02d:%02d",
             tm_info.tm_year + 1900,
             tm_info.tm_mon + 1,
             tm_info.tm_mday,
             tm_info.tm_hour,
             tm_info.tm_min,
             tm_info.tm_sec);
}

static const char *tx_perspective_text(ui_ctx_t *ctx, const transaction_t *tx)
{
    if (tx == NULL) {
        return "?";
    }

    if (ctx != NULL && ctx->own_pubkey != NULL) {
        if (tx->type == TX_TYPE_TRANSFER) {
            if (public_key_equal(&tx->from, ctx->own_pubkey)) {
                return "Envoye";
            }
            if (public_key_equal(&tx->to, ctx->own_pubkey)) {
                return "Recu";
            }
            return "Relais";
        }

        if (tx->type == TX_TYPE_MINT &&
            public_key_equal(&tx->to, ctx->own_pubkey)) {
            return "Credit";
        }
    }

    return (tx->type == TX_TYPE_MINT) ? "Credit" : "Transfert";
}

static lv_color_t tx_perspective_color(ui_ctx_t *ctx, const transaction_t *tx)
{
    if (tx == NULL) {
        return UI_COLOR_TEXT_DIM;
    }

    if (tx->type == TX_TYPE_MINT) {
        return UI_COLOR_SUCCESS;
    }

    if (ctx != NULL && ctx->own_pubkey != NULL) {
        if (public_key_equal(&tx->from, ctx->own_pubkey)) {
            return UI_COLOR_ACCENT;
        }
        if (public_key_equal(&tx->to, ctx->own_pubkey)) {
            return UI_COLOR_SUCCESS;
        }
    }

    return UI_COLOR_TEXT_DIM;
}

static const char *tx_status_text(const transaction_t *tx)
{
    switch (tx->status) {
        case TX_STATUS_LOCKED:    return "LOCKED";
        case TX_STATUS_CONFIRMED: return "CONFIRMED";
        case TX_STATUS_CANCELLED: return "CANCELLED";
        default:                  return "?";
    }
}

static bool history_contains(const transaction_t *txs, uint32_t count,
                             const hash_t *id)
{
    for (uint32_t i = 0; i < count; i++) {
        if (memcmp(txs[i].id.bytes, id->bytes, CRYPTO_HASH_SIZE) == 0) {
            return true;
        }
    }
    return false;
}

static void format_party_label(ui_ctx_t *ctx, const public_key_t *key,
                               char *buf, size_t buf_len)
{
    if (buf == NULL || buf_len == 0) {
        return;
    }
    buf[0] = '\0';

    if (ctx != NULL && key != NULL) {
        if (ctx->own_pubkey != NULL &&
            public_key_equal(key, ctx->own_pubkey)) {
            if (ctx->device_alias != NULL && ctx->device_alias[0] != '\0') {
                snprintf(buf, buf_len, "%s", ctx->device_alias);
            } else {
                snprintf(buf, buf_len, "Moi");
            }
            return;
        }

        if (ctx->state_mutex != NULL &&
            xSemaphoreTake(ctx->state_mutex, pdMS_TO_TICKS(25)) == pdTRUE) {
            if (ctx->peers != NULL && ctx->peer_count != NULL) {
                for (uint32_t i = 0; i < *ctx->peer_count; i++) {
                    if (public_key_equal(key, &ctx->peers[i].public_key)) {
                        if (ctx->peers[i].alias[0] != '\0') {
                            snprintf(buf, buf_len, "%s", ctx->peers[i].alias);
                        }
                        break;
                    }
                }
            }
            xSemaphoreGive(ctx->state_mutex);
        }
    }

    if (buf[0] == '\0' && key != NULL) {
        format_key_short(buf, buf_len, key);
    }
}

static void history_sort_newest_first(transaction_t *txs, uint32_t count)
{
    for (uint32_t i = 1; i < count; i++) {
        transaction_t key = txs[i];
        uint32_t j = i;
        while (j > 0 && txs[j - 1].timestamp < key.timestamp) {
            txs[j] = txs[j - 1];
            j--;
        }
        txs[j] = key;
    }
}

static uint32_t build_history_cache(ui_ctx_t *ctx, uint32_t *out_dag_count,
                                    uint32_t *out_persisted_count)
{
    if (!history_buffers_ready()) {
        if (out_dag_count) *out_dag_count = 0;
        if (out_persisted_count) *out_persisted_count = 0;
        s_history_count = 0;
        return 0;
    }

    uint32_t tmp_count = 0;
    uint32_t persisted_count = 0;

    if (ctx->load_history_txs != NULL) {
        persisted_count = ctx->load_history_txs(s_history_source, HISTORY_MAX_SOURCE);
        if (persisted_count > HISTORY_MAX_SOURCE) {
            persisted_count = HISTORY_MAX_SOURCE;
        }
        tmp_count = persisted_count;
    }

    uint32_t dag_count = 0;
    if (xSemaphoreTake(ctx->state_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        dag_count = ctx->dag ? ctx->dag->count : 0;
        for (uint32_t i = 0;
             ctx->dag && i < ctx->dag->count && tmp_count < HISTORY_MAX_SOURCE;
             i++) {
            const transaction_t *tx = &ctx->dag->transactions[i];
            if (!history_contains(s_history_source, tmp_count, &tx->id)) {
                memcpy(&s_history_source[tmp_count], tx, sizeof(transaction_t));
                tmp_count++;
            }
        }
        xSemaphoreGive(ctx->state_mutex);
    }

    history_sort_newest_first(s_history_source, tmp_count);

    s_history_count = tmp_count;
    if (s_history_count > HISTORY_MAX_DISPLAY) {
        s_history_count = HISTORY_MAX_DISPLAY;
    }
    memcpy(s_history_cache, s_history_source, s_history_count * sizeof(transaction_t));

    if (out_dag_count) *out_dag_count = dag_count;
    if (out_persisted_count) *out_persisted_count = persisted_count;
    return s_history_count;
}

static void add_detail_line(lv_obj_t *parent, const char *label,
                            const char *value, bool mono)
{
    lv_obj_t *title = lv_label_create(parent);
    lv_obj_set_style_text_font(title, ui_theme_font_normal(), 0);
    lv_obj_set_style_text_color(title, UI_COLOR_TEXT_DIM, 0);
    lv_label_set_text(title, label);

    lv_obj_t *body = lv_label_create(parent);
    lv_obj_set_width(body, lv_pct(100));
    lv_obj_set_style_text_font(body, ui_theme_font_normal(), 0);
    lv_obj_set_style_text_color(body, UI_COLOR_TEXT, 0);
    lv_label_set_long_mode(body, LV_LABEL_LONG_WRAP);
    if (mono) {
        lv_obj_set_style_text_letter_space(body, 0, 0);
    }
    lv_label_set_text(body, value);
}

static void show_tx_detail(uint32_t index)
{
    if (index >= s_history_count) {
        return;
    }

    if (s_detail_modal) {
        lv_obj_delete(s_detail_modal);
        s_detail_modal = NULL;
    }

    const transaction_t *tx = &s_history_cache[index];

    s_detail_modal = lv_obj_create(lv_screen_active());
    lv_obj_set_size(s_detail_modal, lv_pct(94), lv_pct(92));
    lv_obj_center(s_detail_modal);
    lv_obj_add_style(s_detail_modal, &ui_style_card, 0);
    lv_obj_set_style_pad_all(s_detail_modal, 8, 0);
    lv_obj_set_style_pad_row(s_detail_modal, 5, 0);
    lv_obj_set_flex_flow(s_detail_modal, LV_FLEX_FLOW_COLUMN);
    lv_obj_add_flag(s_detail_modal, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *top = lv_obj_create(s_detail_modal);
    lv_obj_remove_style_all(top);
    lv_obj_set_size(top, lv_pct(100), 28);
    lv_obj_clear_flag(top, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(top);
    lv_obj_set_style_text_font(title, ui_theme_font_title(), 0);
    lv_obj_set_style_text_color(title, UI_COLOR_TEXT, 0);
    lv_label_set_text(title, "Transaction");
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 0, 0);

    lv_obj_t *close_btn = lv_button_create(top);
    lv_obj_set_size(close_btn, 32, 26);
    lv_obj_add_style(close_btn, &ui_style_btn, 0);
    lv_obj_align(close_btn, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_add_event_cb(close_btn, detail_close_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *close_lbl = lv_label_create(close_btn);
    lv_label_set_text(close_lbl, LV_SYMBOL_CLOSE);
    lv_obj_center(close_lbl);

    char amount[32];
    snprintf(amount, sizeof(amount), "%"PRIu32, tx->amount);
    char fee[32];
    snprintf(fee, sizeof(fee), "%"PRIu32, tx->fee);
    char seq[32];
    snprintf(seq, sizeof(seq), "%"PRIu32, tx->seq);
    char date[40];
    format_tx_datetime(date, sizeof(date), tx->timestamp);
    char from[COMM_MSG_ALIAS_MAX + 16];
    char to[COMM_MSG_ALIAS_MAX + 16];
    format_party_label(s_active_ctx, &tx->from, from, sizeof(from));
    format_party_label(s_active_ctx, &tx->to, to, sizeof(to));

    add_detail_line(s_detail_modal, "Type", tx_perspective_text(s_active_ctx, tx), false);
    add_detail_line(s_detail_modal, "Statut", tx_status_text(tx), false);
    add_detail_line(s_detail_modal, "Montant", amount, false);
    add_detail_line(s_detail_modal, "Frais", fee, false);
    add_detail_line(s_detail_modal, "Date / heure", date, false);
    add_detail_line(s_detail_modal, "From", from, false);
    add_detail_line(s_detail_modal, "To", to, false);
    add_detail_line(s_detail_modal, "Seq", seq, false);
}

static void tx_row_cb(lv_event_t *e)
{
    uint32_t idx = (uint32_t)(uintptr_t)lv_event_get_user_data(e);
    show_tx_detail(idx);
}

/* ================================================================
 * Interface ecran
 * ================================================================ */

static lv_obj_t *screen_create(ui_ctx_t *ctx)
{
    s_active_ctx = ctx;
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

    s_active_ctx = ctx;
    const bool small = ctx->is_small_screen;
    uint32_t dag_count = 0;
    uint32_t persisted_count = 0;
    uint32_t now_tick = lv_tick_get();

    uint32_t count = build_history_cache(ctx, &dag_count, &persisted_count);

    /*
     * Reconstruire si le nombre change, ou toutes les 2 s pour rafraichir
     * les temps relatifs et les changements de statut sans variation de count.
     */
    bool timed_refresh = (now_tick - s_last_refresh_tick) > 2000U;
    if (count == s_last_count &&
        persisted_count == s_last_persisted_count &&
        !timed_refresh) {
        return;
    }
    s_last_count = count;
    s_last_persisted_count = persisted_count;
    s_last_refresh_tick = now_tick;

    /* Nettoyer la liste */
    lv_obj_clean(s_tx_list);

    if (count == 0) {
        /* Aucun historique durable ni DAG RAM : afficher le message */
        lv_obj_clear_flag(s_empty_label, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_tx_list, LV_OBJ_FLAG_HIDDEN);
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

    /* TX deja triees du plus recent au plus ancien dans s_history_cache */
    for (uint32_t i = 0; i < count; i++) {
        const transaction_t *tx = &s_history_cache[i];

            const char *type_str = tx_perspective_text(ctx, tx);
            lv_color_t type_color = tx_perspective_color(ctx, tx);

            /* Formater le timestamp relatif */
            char time_str[24];
            format_relative_time(time_str, sizeof(time_str),
                                 tx->timestamp, now_ms);

            if (small) {
                /* --------------------------------------------------
                 * Petit ecran : 1 ligne par TX
                 * Format : "Envoye  500  2 min"
                 * -------------------------------------------------- */
                lv_obj_t *row = lv_obj_create(s_tx_list);
                lv_obj_remove_style_all(row);
                lv_obj_add_style(row, &ui_style_card, 0);
                lv_obj_set_size(row, lv_pct(100), 30);
                lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
                lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
                lv_obj_add_event_cb(row, tx_row_cb, LV_EVENT_CLICKED,
                                    (void *)(uintptr_t)i);

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
                lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
                lv_obj_add_event_cb(card, tx_row_cb, LV_EVENT_CLICKED,
                                    (void *)(uintptr_t)i);

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
                char from_str[COMM_MSG_ALIAS_MAX + 16];
                char to_str[COMM_MSG_ALIAS_MAX + 16];
                char addr_line[(COMM_MSG_ALIAS_MAX * 2) + 40];
                format_party_label(ctx, &tx->from, from_str, sizeof(from_str));
                format_party_label(ctx, &tx->to,   to_str,   sizeof(to_str));
                snprintf(addr_line, sizeof(addr_line), "%s -> %s",
                         from_str, to_str);

                lv_obj_t *addr_lbl = lv_label_create(card);
                lv_obj_set_style_text_font(addr_lbl, ui_theme_font_normal(), 0);
                lv_obj_set_style_text_color(addr_lbl, UI_COLOR_TEXT_DIM, 0);
                lv_label_set_text(addr_lbl, addr_line);
                lv_obj_align(addr_lbl, LV_ALIGN_BOTTOM_LEFT, 8, -4);
            }
    }

    (void)dag_count;
}

static void screen_destroy(void)
{
    if (s_screen) {
        lv_obj_delete(s_screen);
        s_screen      = NULL;
        s_tx_list     = NULL;
        s_empty_label = NULL;
        s_last_count  = UINT32_MAX;
        s_active_ctx  = NULL;
    }
}

ui_screen_handler_t ui_screen_history_handler = {
    .create  = screen_create,
    .update  = screen_update,
    .destroy = screen_destroy,
};
