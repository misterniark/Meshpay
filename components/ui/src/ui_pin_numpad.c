/**
 * @file ui_pin_numpad.c
 * @brief Clavier numerique pour saisie de PIN (CYD grand ecran 320x240).
 *
 * Affiche un pave numerique 3x4 avec indicateur de progression (dots).
 * Chaque appui sur un chiffre remplit un dot ; quand les 4 chiffres
 * sont saisis, le callback est appele avec le PIN.
 */

#include "ui/ui_pin.h"
#include "ui/ui_theme.h"

#include <string.h>

/* ================================================================
 * Etat interne du numpad
 * ================================================================ */

/** Contexte du widget numpad, stocke dans user_data de l'overlay */
typedef struct {
    ui_pin_callback_t callback;
    void             *user_data;
    uint8_t           pin[UI_PIN_LENGTH]; /* Chiffres saisis */
    uint8_t           pos;               /* Position courante (0-3) */
    lv_obj_t         *dots[UI_PIN_LENGTH]; /* Indicateurs visuels */
} pin_numpad_ctx_t;

static pin_numpad_ctx_t s_numpad_ctx;

/* ================================================================
 * Callbacks
 * ================================================================ */

/**
 * Callback appele quand l'utilisateur appuie sur un bouton du numpad.
 *
 * Les boutons portent un texte "0"-"9", "C" (effacer) ou "<" (backspace).
 * A chaque chiffre saisi, le dot correspondant passe en couleur accent.
 * Quand 4 chiffres sont saisis, le callback PIN est appele.
 */
static void numpad_btn_cb(lv_event_t *e)
{
    lv_obj_t *btn = lv_event_get_target(e);
    const char *txt = lv_label_get_text(lv_obj_get_child(btn, 0));

    if (strcmp(txt, "C") == 0) {
        /* Effacer tout */
        s_numpad_ctx.pos = 0;
        for (int i = 0; i < UI_PIN_LENGTH; i++) {
            lv_obj_set_style_bg_color(s_numpad_ctx.dots[i],
                                       UI_COLOR_CARD, 0);
        }
        return;
    }

    if (strcmp(txt, LV_SYMBOL_BACKSPACE) == 0) {
        /* Supprimer le dernier chiffre */
        if (s_numpad_ctx.pos > 0) {
            s_numpad_ctx.pos--;
            lv_obj_set_style_bg_color(
                s_numpad_ctx.dots[s_numpad_ctx.pos],
                UI_COLOR_CARD, 0);
        }
        return;
    }

    /* Chiffre 0-9 */
    if (s_numpad_ctx.pos < UI_PIN_LENGTH) {
        uint8_t digit = (uint8_t)(txt[0] - '0');
        s_numpad_ctx.pin[s_numpad_ctx.pos] = digit;

        /* Marquer le dot comme rempli */
        lv_obj_set_style_bg_color(
            s_numpad_ctx.dots[s_numpad_ctx.pos],
            UI_COLOR_ACCENT, 0);

        s_numpad_ctx.pos++;

        /* PIN complet ? */
        if (s_numpad_ctx.pos == UI_PIN_LENGTH) {
            if (s_numpad_ctx.callback) {
                s_numpad_ctx.callback(s_numpad_ctx.pin,
                                       s_numpad_ctx.user_data);
            }
        }
    }
}

/* ================================================================
 * Creation du widget
 * ================================================================ */

/**
 * Cree un bouton du numpad avec le texte donne.
 */
static lv_obj_t *create_btn(lv_obj_t *parent, const char *text,
                             lv_coord_t w, lv_coord_t h)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_size(btn, w, h);
    lv_obj_add_style(btn, &ui_style_btn, 0);
    lv_obj_add_event_cb(btn, numpad_btn_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *label = lv_label_create(btn);
    lv_label_set_text(label, text);
    lv_obj_center(label);

    return btn;
}

lv_obj_t *ui_pin_numpad_create(lv_obj_t *parent, const char *title,
                                ui_pin_callback_t callback, void *user_data)
{
    /* Reinitialiser le contexte */
    memset(&s_numpad_ctx, 0, sizeof(s_numpad_ctx));
    s_numpad_ctx.callback  = callback;
    s_numpad_ctx.user_data = user_data;

    /* Overlay plein ecran */
    lv_obj_t *overlay = lv_obj_create(parent ? parent : lv_screen_active());
    lv_obj_set_size(overlay, LV_PCT(100), LV_PCT(100));
    lv_obj_add_style(overlay, &ui_style_screen, 0);
    lv_obj_set_style_pad_all(overlay, 10, 0);
    lv_obj_set_flex_flow(overlay, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(overlay,
                          LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    /* Titre */
    lv_obj_t *lbl_title = lv_label_create(overlay);
    lv_obj_add_style(lbl_title, &ui_style_title, 0);
    lv_label_set_text(lbl_title, title);

    /* Indicateurs dots (4 cercles) */
    lv_obj_t *dot_row = lv_obj_create(overlay);
    lv_obj_set_size(dot_row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(dot_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(dot_row,
                          LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(dot_row, 12, 0);
    lv_obj_set_style_bg_opa(dot_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(dot_row, 0, 0);

    for (int i = 0; i < UI_PIN_LENGTH; i++) {
        lv_obj_t *dot = lv_obj_create(dot_row);
        lv_obj_set_size(dot, 20, 20);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(dot, UI_COLOR_CARD, 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(dot, UI_COLOR_TEXT, 0);
        lv_obj_set_style_border_width(dot, 1, 0);
        s_numpad_ctx.dots[i] = dot;
    }

    /* Grille de boutons : taille adaptee au grand ecran (320x240) */
    const lv_coord_t btn_w = 60;
    const lv_coord_t btn_h = 36;

    /* Disposition grille 4 lignes x 3 colonnes */
    static const char *btn_map[4][3] = {
        {"1", "2", "3"},
        {"4", "5", "6"},
        {"7", "8", "9"},
        {"C", "0", LV_SYMBOL_BACKSPACE},
    };

    for (int row = 0; row < 4; row++) {
        lv_obj_t *row_obj = lv_obj_create(overlay);
        lv_obj_set_size(row_obj, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(row_obj, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row_obj,
                              LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(row_obj, 8, 0);
        lv_obj_set_style_bg_opa(row_obj, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(row_obj, 0, 0);
        lv_obj_set_style_pad_top(row_obj, 2, 0);
        lv_obj_set_style_pad_bottom(row_obj, 2, 0);

        for (int col = 0; col < 3; col++) {
            create_btn(row_obj, btn_map[row][col], btn_w, btn_h);
        }
    }

    return overlay;
}
