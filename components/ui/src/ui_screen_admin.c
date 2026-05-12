/**
 * @file ui_screen_admin.c
 * @brief Ecran Menu admin — Acces aux fonctions d'administration (maitre uniquement).
 *
 * Affiche une grille de boutons vers les sous-ecrans admin :
 * - Creer credits (mint)
 * - Broadcast (message texte, grand ecran uniquement)
 * - Scanner (ping devices)
 * - Renommer device (futur, desactive)
 * - Configurer forward (futur, desactive)
 *
 * Grand ecran (CYD 320x240) : grille 2 colonnes x 3 lignes
 * Petit ecran (Waveshare 320x172 paysage) : grille 2 colonnes compacte
 */

#include "ui/ui_screens.h"
#include "ui/ui_state.h"
#include "ui/ui_theme.h"
#include "ui/ui_manager.h"

static lv_obj_t *s_screen = NULL;

/* ----------------------------------------------------------------
 * Callbacks de navigation
 * ---------------------------------------------------------------- */

/** Retour a l'ecran precedent */
static void back_cb(lv_event_t *e)
{
    (void)e;
    ui_manager_back();
}

/** Navigation vers l'ecran de creation de credits */
static void mint_cb(lv_event_t *e)
{
    (void)e;
    ui_manager_show(UI_SCREEN_MINT);
}

/** Navigation vers l'ecran d'envoi de broadcast */
static void message_cb(lv_event_t *e)
{
    (void)e;
    ui_manager_show(UI_SCREEN_MESSAGE);
}

/** Navigation vers l'ecran de scan des devices */
static void scan_cb(lv_event_t *e)
{
    (void)e;
    ui_manager_show(UI_SCREEN_SCAN);
}

/** Navigation vers l'ecran de renommage d'un device */
static void rename_cb(lv_event_t *e)
{
    (void)e;
    ui_manager_show(UI_SCREEN_RENAME);
}

/** Navigation vers l'ecran de configuration forward */
static void forward_cb(lv_event_t *e)
{
    (void)e;
    ui_manager_show(UI_SCREEN_FORWARD);
}

/* ----------------------------------------------------------------
 * Utilitaire : creation d'un bouton de menu
 * ---------------------------------------------------------------- */

/**
 * Cree un bouton avec texte, style commun, et callback optionnel.
 *
 * @param parent   Conteneur parent (la grille)
 * @param text     Libelle du bouton
 * @param cb       Callback au clic (NULL = bouton desactive)
 * @param small    true si petit ecran
 * @return         Pointeur vers le bouton cree
 */
static lv_obj_t *create_menu_btn(lv_obj_t *parent, const char *text,
                                  lv_event_cb_t cb, bool small)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_add_style(btn, &ui_style_btn, 0);
    lv_obj_set_size(btn, small ? lv_pct(44) : lv_pct(44), small ? 36 : 50);

    lv_obj_t *label = lv_label_create(btn);
    lv_obj_set_style_text_font(label, ui_theme_font_normal(), 0);
    lv_obj_set_style_text_color(label, UI_COLOR_TEXT, 0);
    lv_label_set_text(label, text);
    lv_obj_center(label);

    if (cb) {
        lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
    } else {
        /* Bouton desactive : opacite reduite pour indiquer l'indisponibilite */
        lv_obj_add_state(btn, LV_STATE_DISABLED);
        lv_obj_set_style_opa(btn, LV_OPA_50, 0);
    }

    return btn;
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

    /* Bouton retour */
    lv_obj_t *btn_back = lv_button_create(header);
    lv_obj_set_size(btn_back, small ? 40 : 50, small ? 26 : 32);
    lv_obj_add_style(btn_back, &ui_style_btn, 0);
    lv_obj_add_event_cb(btn_back, back_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *back_label = lv_label_create(btn_back);
    lv_label_set_text(back_label, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_color(back_label, UI_COLOR_TEXT, 0);
    lv_obj_center(back_label);

    /* Titre */
    lv_obj_t *title = lv_label_create(header);
    lv_obj_add_style(title, &ui_style_title, 0);
    lv_label_set_text(title, "Admin");
    lv_obj_set_style_pad_left(title, 8, 0);

    /* --- Conteneur pour la grille de boutons --- */
    lv_obj_t *cont = lv_obj_create(s_screen);
    lv_obj_set_style_bg_opa(cont, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(cont, 0, 0);
    lv_obj_set_style_pad_all(cont, small ? 4 : 10, 0);

    if (small) {
        /* Petit ecran paysage (320x172) : grille 2 colonnes compacte */
        lv_obj_set_size(cont, lv_pct(100),
                        ctx->screen_h - 42);
        lv_obj_align(cont, LV_ALIGN_TOP_MID, 0, 38);
        lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_ROW_WRAP);
        lv_obj_set_flex_align(cont, LV_FLEX_ALIGN_SPACE_EVENLY,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);
        lv_obj_set_style_pad_row(cont, 6, 0);
        lv_obj_set_style_pad_column(cont, 6, 0);
    } else {
        /* Grand ecran : grille 2 colonnes x 3 lignes */
        lv_obj_set_size(cont, lv_pct(100),
                        ctx->screen_h - 54);
        lv_obj_align(cont, LV_ALIGN_TOP_MID, 0, 48);
        lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_ROW_WRAP);
        lv_obj_set_flex_align(cont, LV_FLEX_ALIGN_SPACE_EVENLY,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);
        lv_obj_set_style_pad_row(cont, 10, 0);
        lv_obj_set_style_pad_column(cont, 10, 0);
    }

    /* --- Boutons du menu admin --- */
    create_menu_btn(cont, "Creer credits",      mint_cb,    small);

    /* Broadcast desactive sur petit ecran : le clavier virtuel ne tient
     * pas dans 172px de hauteur. Le device continue neanmoins a recevoir
     * et propager les broadcasts en single-hop via LoRa. */
    create_menu_btn(cont, "Broadcast",
                    small ? NULL : message_cb, small);

    create_menu_btn(cont, "Scanner",             scan_cb,    small);
    /* Renommer desactive sur petit ecran : necessite un clavier virtuel
     * qui ne tient pas dans 172px de hauteur. */
    create_menu_btn(cont, "Renommer device",
                    small ? NULL : rename_cb, small);
    create_menu_btn(cont, "Configurer forward",  forward_cb, small);

    return s_screen;
}

static void screen_update(ui_ctx_t *ctx)
{
    /* Aucune donnee dynamique a mettre a jour sur cet ecran */
    (void)ctx;
}

static void screen_destroy(void)
{
    if (s_screen) {
        lv_obj_delete(s_screen);
        s_screen = NULL;
    }
}

ui_screen_handler_t ui_screen_admin_handler = {
    .create  = screen_create,
    .update  = screen_update,
    .destroy = screen_destroy,
};
