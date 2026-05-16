/**
 * @file ui_screen_settings.c
 * @brief Ecran Parametres — Informations device, monnaie et statistiques.
 *
 * Affiche trois cartes d'information :
 * - Info device : alias, cle publique (hex tronque), resolution
 * - Monnaie : nom et ID
 * - Statistiques : nombre de TX dans le DAG, etat auto-forward
 *
 * Un bouton "Admin" permet de naviguer vers l'ecran d'administration.
 *
 * Layouts :
 * - Grand ecran (CYD 320x240) : 2 colonnes de cartes
 * - Petit ecran (Waveshare 320x172 paysage) : grille 2 colonnes, scrollable
 */

#include "ui/ui_screens.h"
#include "ui/ui_state.h"
#include "ui/ui_theme.h"
#include "ui/ui_manager.h"
#include "ui/ui_pin.h"

#include "dag/dag.h"
#include "crypto/crypto_types.h"

#include <stdio.h>
#include <string.h>

/* Ecran racine */
static lv_obj_t *s_screen = NULL;

/* Labels dynamiques mis a jour par screen_update() */
static lv_obj_t *s_lbl_alias      = NULL;
static lv_obj_t *s_lbl_pubkey     = NULL;
static lv_obj_t *s_lbl_resolution = NULL;
static lv_obj_t *s_lbl_currency   = NULL;
static lv_obj_t *s_lbl_cur_id     = NULL;
static lv_obj_t *s_lbl_tx_count   = NULL;
static lv_obj_t *s_lbl_forward    = NULL;

/* Label et timer pour le message d'erreur acces admin */
static lv_obj_t   *s_lbl_admin_err   = NULL;
static lv_timer_t *s_admin_err_timer = NULL;

/* ================================================================
 * Callbacks
 * ================================================================ */

/**
 * Callback du bouton retour : revient a l'ecran precedent (HOME).
 */
static void back_cb(lv_event_t *e)
{
    (void)e;
    ui_manager_back();
}

/**
 * Callback du timer d'erreur admin : masque le label apres 2 secondes.
 */
static void admin_err_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    if (s_lbl_admin_err) {
        lv_obj_add_flag(s_lbl_admin_err, LV_OBJ_FLAG_HIDDEN);
    }
    s_admin_err_timer = NULL;
}

/**
 * Callback du bouton Admin : navigue vers l'ecran d'administration.
 *
 * Verifie que le device est un maitre (mint_authority) avant
 * d'autoriser l'acces. Affiche un message d'erreur si non autorise.
 */
/*
 * [F-UI-001] Callback de vérification du PIN avant accès à l'écran
 * Admin. Sans cette garde, n'importe qui ayant emprunté un device
 * maître quelques secondes pouvait accéder aux écrans Mint, Broadcast,
 * Forward (création monétaire arbitraire, diffusion de message).
 */
static void admin_pin_cb(const uint8_t pin[UI_PIN_LENGTH], void *user_data)
{
    ui_ctx_t *ctx = (ui_ctx_t *)user_data;
    ui_pin_close();
    if (!ctx || !ctx->storage) return;

    ui_pin_result_t res = ui_pin_verify(pin, ctx->storage);
    if (res == UI_PIN_OK) {
        ui_manager_show(UI_SCREEN_ADMIN);
    } else {
        if (s_lbl_admin_err) {
            const char *msg = (res == UI_PIN_BLOCKED)
                                ? "Trop de tentatives"
                                : (res == UI_PIN_COOLDOWN)
                                    ? "Patientez..."
                                    : "PIN incorrect";
            lv_label_set_text(s_lbl_admin_err, msg);
            lv_obj_clear_flag(s_lbl_admin_err, LV_OBJ_FLAG_HIDDEN);
            if (s_admin_err_timer) {
                lv_timer_delete(s_admin_err_timer);
            }
            s_admin_err_timer = lv_timer_create(admin_err_timer_cb, 2000, NULL);
            lv_timer_set_repeat_count(s_admin_err_timer, 1);
        }
    }
}

static void admin_cb(lv_event_t *e)
{
    ui_ctx_t *ctx = (ui_ctx_t *)lv_event_get_user_data(e);

    /* Verification du droit d'acces admin */
    if (!ctx || !ctx->is_master) {
        /* Afficher le message d'erreur dans le label dedie */
        if (s_lbl_admin_err) {
            lv_label_set_text(s_lbl_admin_err, "Acces refuse : non maitre");
            lv_obj_clear_flag(s_lbl_admin_err, LV_OBJ_FLAG_HIDDEN);
            if (s_admin_err_timer) {
                lv_timer_delete(s_admin_err_timer);
            }
            s_admin_err_timer = lv_timer_create(admin_err_timer_cb, 2000, NULL);
            lv_timer_set_repeat_count(s_admin_err_timer, 1);
        }
        return;
    }

    /*
     * [F-UI-001] Demander la saisie du PIN avant la navigation. Le
     * callback vérifie le PIN et ne navigue que si UI_PIN_OK.
     * Si aucun PIN n'est configuré (premier boot, setup pas encore
     * fait), on accepte directement pour ne pas bloquer le bootstrap.
     */
    if (ctx->storage && ui_pin_is_configured(ctx->storage)) {
        ui_pin_show(NULL, "PIN admin", admin_pin_cb, ctx, ctx->is_small_screen);
    } else {
        ui_manager_show(UI_SCREEN_ADMIN);
    }
}

/* ================================================================
 * Fonctions utilitaires
 * ================================================================ */

/**
 * Formate une cle publique en hex tronque : 8 premiers + "..." + 8 derniers.
 * Exemple : "a1b2c3d4...e5f6g7h8"
 *
 * @param key    Cle publique a formater
 * @param buf    Buffer de sortie (min 20 caracteres)
 * @param buflen Taille du buffer
 */
static void format_pubkey_short(const public_key_t *key, char *buf, size_t buflen)
{
    if (!key || buflen < 20) {
        snprintf(buf, buflen, "???");
        return;
    }

    /* 4 premiers octets = 8 caracteres hex */
    char prefix[9];
    snprintf(prefix, sizeof(prefix), "%02x%02x%02x%02x",
             key->bytes[0], key->bytes[1], key->bytes[2], key->bytes[3]);

    /* 4 derniers octets = 8 caracteres hex */
    char suffix[9];
    snprintf(suffix, sizeof(suffix), "%02x%02x%02x%02x",
             key->bytes[CRYPTO_PUBLIC_KEY_SIZE - 4],
             key->bytes[CRYPTO_PUBLIC_KEY_SIZE - 3],
             key->bytes[CRYPTO_PUBLIC_KEY_SIZE - 2],
             key->bytes[CRYPTO_PUBLIC_KEY_SIZE - 1]);

    snprintf(buf, buflen, "%s...%s", prefix, suffix);
}

/**
 * Cree un label avec un prefixe (titre de champ) dans une carte.
 * Retourne le label pour pouvoir le mettre a jour dans update().
 *
 * @param parent Carte parente
 * @param prefix Texte du prefixe (ex: "Alias :")
 * @param value  Valeur initiale affichee
 * @return       Le label de la valeur (pour mise a jour dynamique)
 */
static lv_obj_t *add_info_row(lv_obj_t *parent, const char *prefix,
                              const char *value)
{
    /* Conteneur pour une ligne d'info */
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 4, 0);

    /* Label du prefixe (couleur attenuee) */
    lv_obj_t *lbl_prefix = lv_label_create(row);
    lv_obj_set_style_text_font(lbl_prefix, ui_theme_font_normal(), 0);
    lv_obj_set_style_text_color(lbl_prefix, UI_COLOR_TEXT_DIM, 0);
    lv_label_set_text(lbl_prefix, prefix);

    /* Label de la valeur (couleur principale) */
    lv_obj_t *lbl_value = lv_label_create(row);
    lv_obj_set_style_text_font(lbl_value, ui_theme_font_normal(), 0);
    lv_obj_set_style_text_color(lbl_value, UI_COLOR_TEXT, 0);
    lv_label_set_long_mode(lbl_value, LV_LABEL_LONG_CLIP);
    lv_label_set_text(lbl_value, value);

    return lbl_value;
}

/**
 * Cree une carte avec un titre.
 *
 * @param parent     Conteneur parent
 * @param title      Titre de la carte
 * @param small      true si petit ecran (ajuste la taille)
 * @param width      Largeur de la carte (LV_PCT ou pixels)
 * @return           Objet carte cree
 */
static lv_obj_t *create_card(lv_obj_t *parent, const char *title,
                             bool small, int32_t width)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_add_style(card, &ui_style_card, 0);
    lv_obj_set_width(card, width);
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(card, small ? 6 : 10, 0);
    lv_obj_set_style_pad_row(card, small ? 3 : 5, 0);

    /* Titre de la carte */
    lv_obj_t *lbl_title = lv_label_create(card);
    lv_obj_set_style_text_font(lbl_title, ui_theme_font_normal(), 0);
    lv_obj_set_style_text_color(lbl_title, UI_COLOR_ACCENT, 0);
    lv_label_set_text(lbl_title, title);

    return card;
}

/* ================================================================
 * Handler d'ecran
 * ================================================================ */

static lv_obj_t *screen_create(ui_ctx_t *ctx)
{
    s_screen = lv_obj_create(NULL);
    lv_obj_add_style(s_screen, &ui_style_screen, 0);

    const bool small = ctx->is_small_screen;

    /* --- Header : titre + bouton retour --- */
    lv_obj_t *header = lv_obj_create(s_screen);
    lv_obj_add_style(header, &ui_style_header, 0);
    lv_obj_set_width(header, LV_PCT(100));
    lv_obj_set_height(header, small ? 32 : 40);
    lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(header, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_hor(header, small ? 6 : 10, 0);
    lv_obj_align(header, LV_ALIGN_TOP_LEFT, 0, 0);

    /* Bouton retour */
    lv_obj_t *btn_back = lv_button_create(header);
    lv_obj_set_size(btn_back, small ? 30 : 40, small ? 24 : 30);
    lv_obj_set_style_bg_opa(btn_back, LV_OPA_TRANSP, 0);
    lv_obj_set_style_shadow_width(btn_back, 0, 0);
    lv_obj_add_event_cb(btn_back, back_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *back_label = lv_label_create(btn_back);
    lv_obj_set_style_text_font(back_label, ui_theme_font_normal(), 0);
    lv_obj_set_style_text_color(back_label, UI_COLOR_TEXT, 0);
    lv_label_set_text(back_label, LV_SYMBOL_LEFT);
    lv_obj_center(back_label);

    /* Titre du header */
    lv_obj_t *title = lv_label_create(header);
    lv_obj_add_style(title, &ui_style_title, 0);
    lv_label_set_text(title, "Parametres");

    /* --- Zone de contenu scrollable --- */
    lv_obj_t *content = lv_obj_create(s_screen);
    lv_obj_remove_style_all(content);
    lv_obj_set_width(content, LV_PCT(100));
    /* Hauteur = tout l'espace sous le header */
    lv_obj_set_height(content, ctx->screen_h - (small ? 32 : 40));
    lv_obj_align(content, LV_ALIGN_TOP_LEFT, 0, small ? 32 : 40);
    lv_obj_set_style_pad_all(content, small ? 6 : 10, 0);
    lv_obj_set_style_pad_row(content, small ? 6 : 8, 0);
    lv_obj_set_style_pad_column(content, small ? 6 : 8, 0);
    lv_obj_add_flag(content, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(content, LV_SCROLLBAR_MODE_AUTO);

    if (small) {
        /* Petit ecran paysage (320x172) : grille 2 colonnes scrollable */
        lv_obj_set_flex_flow(content, LV_FLEX_FLOW_ROW_WRAP);
        lv_obj_set_flex_align(content, LV_FLEX_ALIGN_START,
                              LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    } else {
        /* Grand ecran : grille 2 colonnes avec wrap */
        lv_obj_set_flex_flow(content, LV_FLEX_FLOW_ROW_WRAP);
        lv_obj_set_flex_align(content, LV_FLEX_ALIGN_START,
                              LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    }

    /* Largeur des cartes selon la taille d'ecran */
    const int32_t card_w = small ? LV_PCT(48) : LV_PCT(48);

    /* ---- Carte "Info device" ---- */
    {
        lv_obj_t *card = create_card(content, "Info device", small, card_w);

        /* Alias du device */
        char alias_buf[32];
        if (ctx->device_alias && ctx->device_alias_len &&
            *ctx->device_alias_len > 0) {
            snprintf(alias_buf, sizeof(alias_buf), "%.*s",
                     (int)*ctx->device_alias_len, ctx->device_alias);
        } else {
            snprintf(alias_buf, sizeof(alias_buf), "(non defini)");
        }
        s_lbl_alias = add_info_row(card, "Alias :", alias_buf);

        /* Cle publique (hex tronque) */
        char key_buf[20];
        if (ctx->own_pubkey) {
            format_pubkey_short(ctx->own_pubkey, key_buf,
                                sizeof(key_buf));
        } else {
            snprintf(key_buf, sizeof(key_buf), "???");
        }
        s_lbl_pubkey = add_info_row(card, "Cle :", key_buf);

        /* Resolution ecran */
        char res_buf[16];
        snprintf(res_buf, sizeof(res_buf), "%ux%u",
                 ctx->screen_w, ctx->screen_h);
        s_lbl_resolution = add_info_row(card, "Ecran :", res_buf);
    }

    /* ---- Carte "Monnaie" ---- */
    {
        lv_obj_t *card = create_card(content, "Monnaie", small, card_w);

        /* Nom de la monnaie */
        const char *cur_name = (ctx->currency) ? ctx->currency->name : "---";
        s_lbl_currency = add_info_row(card, "Nom :", cur_name);

        /* ID de la monnaie (hex) */
        char id_buf[12];
        if (ctx->currency) {
            snprintf(id_buf, sizeof(id_buf), "0x%08X",
                     (unsigned)ctx->currency->currency_id);
        } else {
            snprintf(id_buf, sizeof(id_buf), "---");
        }
        s_lbl_cur_id = add_info_row(card, "ID :", id_buf);
    }

    /* ---- Carte "Statistiques" ---- */
    {
        lv_obj_t *card = create_card(content, "Statistiques", small, card_w);

        /* Nombre de TX dans le DAG */
        char tx_buf[16];
        if (ctx->dag) {
            snprintf(tx_buf, sizeof(tx_buf), "%u",
                     (unsigned)dag_count(ctx->dag));
        } else {
            snprintf(tx_buf, sizeof(tx_buf), "0");
        }
        s_lbl_tx_count = add_info_row(card, "TX :", tx_buf);

        /* Etat auto-forward */
        char fwd_buf[48];
        if (ctx->forward_interval_min && *ctx->forward_interval_min > 0) {
            snprintf(fwd_buf, sizeof(fwd_buf), "Actif (%u min)",
                     (unsigned)*ctx->forward_interval_min);
        } else {
            snprintf(fwd_buf, sizeof(fwd_buf), "Inactif");
        }
        s_lbl_forward = add_info_row(card, "Forward :", fwd_buf);
    }

    /* ---- Bouton "Admin" ---- */
    lv_obj_t *btn_admin = lv_button_create(content);
    lv_obj_add_style(btn_admin, &ui_style_btn, 0);
    lv_obj_set_size(btn_admin, small ? LV_PCT(100) : LV_PCT(48),
                    small ? 38 : 44);
    lv_obj_add_event_cb(btn_admin, admin_cb, LV_EVENT_CLICKED, ctx);

    lv_obj_t *admin_label = lv_label_create(btn_admin);
    lv_obj_set_style_text_color(admin_label, UI_COLOR_TEXT, 0);
    lv_obj_set_style_text_font(admin_label, ui_theme_font_normal(), 0);
    lv_label_set_text(admin_label, "Admin");
    lv_obj_center(admin_label);

    /* Label d'erreur acces admin (masque par defaut) */
    s_lbl_admin_err = lv_label_create(content);
    lv_obj_set_style_text_font(s_lbl_admin_err, ui_theme_font_normal(), 0);
    lv_obj_set_style_text_color(s_lbl_admin_err, lv_color_hex(0xCC3333), 0);
    lv_label_set_text(s_lbl_admin_err, "");
    lv_obj_add_flag(s_lbl_admin_err, LV_OBJ_FLAG_HIDDEN);

    return s_screen;
}

static void screen_update(ui_ctx_t *ctx)
{
    if (!s_screen) {
        return;
    }

    /* Verrouillage du mutex pour lire les donnees partagees */
    if (xSemaphoreTake(ctx->state_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return;
    }

    /* Mise a jour de l'alias */
    if (s_lbl_alias) {
        if (ctx->device_alias && ctx->device_alias_len &&
            *ctx->device_alias_len > 0) {
            char buf[32];
            snprintf(buf, sizeof(buf), "%.*s",
                     (int)*ctx->device_alias_len, ctx->device_alias);
            lv_label_set_text(s_lbl_alias, buf);
        } else {
            lv_label_set_text(s_lbl_alias, "(non defini)");
        }
    }

    /* Mise a jour de la cle publique */
    if (s_lbl_pubkey && ctx->own_pubkey) {
        char key_buf[20];
        format_pubkey_short(ctx->own_pubkey, key_buf,
                            sizeof(key_buf));
        lv_label_set_text(s_lbl_pubkey, key_buf);
    }

    /* Mise a jour du nombre de TX */
    if (s_lbl_tx_count && ctx->dag) {
        char tx_buf[16];
        snprintf(tx_buf, sizeof(tx_buf), "%u",
                 (unsigned)dag_count(ctx->dag));
        lv_label_set_text(s_lbl_tx_count, tx_buf);
    }

    /* Mise a jour de l'etat auto-forward */
    if (s_lbl_forward) {
        char fwd_buf[48];
        if (ctx->forward_interval_min && *ctx->forward_interval_min > 0) {
            snprintf(fwd_buf, sizeof(fwd_buf), "Actif (%u min)",
                     (unsigned)*ctx->forward_interval_min);
        } else {
            snprintf(fwd_buf, sizeof(fwd_buf), "Inactif");
        }
        lv_label_set_text(s_lbl_forward, fwd_buf);
    }

    /* Liberation du mutex */
    xSemaphoreGive(ctx->state_mutex);
}

static void screen_destroy(void)
{
    /* Annuler le timer d'erreur admin s'il est actif */
    s_lbl_admin_err = NULL;
    if (s_admin_err_timer) {
        lv_timer_delete(s_admin_err_timer);
        s_admin_err_timer = NULL;
    }

    if (s_screen) {
        lv_obj_delete(s_screen);
        s_screen = NULL;
    }

    /* Reinitialisation des references aux labels dynamiques */
    s_lbl_alias      = NULL;
    s_lbl_pubkey     = NULL;
    s_lbl_resolution = NULL;
    s_lbl_currency   = NULL;
    s_lbl_cur_id     = NULL;
    s_lbl_tx_count   = NULL;
    s_lbl_forward    = NULL;
}

ui_screen_handler_t ui_screen_settings_handler = {
    .create  = screen_create,
    .update  = screen_update,
    .destroy = screen_destroy,
};
