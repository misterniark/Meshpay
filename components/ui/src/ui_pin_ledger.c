/**
 * @file ui_pin_ledger.c
 * @brief Saisie de PIN style Ledger pour petit ecran (Waveshare 172x320).
 *
 * Layout paysage (320x172) :
 * - Titre + indicateurs des 4 chiffres en haut
 * - Zone centrale : bouton UP, grand chiffre, bouton DOWN en ligne
 * - Bouton "Valider" en bas pour confirmer le chiffre courant
 */

#include "ui/ui_pin.h"
#include "ui/ui_theme.h"

#include <string.h>

/* ================================================================
 * Etat interne du widget Ledger
 * ================================================================ */

/** Contexte du widget Ledger */
typedef struct {
    ui_pin_callback_t callback;
    void             *user_data;
    uint8_t           pin[UI_PIN_LENGTH]; /* Chiffres saisis */
    uint8_t           pos;               /* Position courante (0-3) */
    lv_obj_t         *digit_labels[UI_PIN_LENGTH]; /* Labels des chiffres */
    lv_obj_t         *current_digit_label; /* Grand chiffre central */
} pin_ledger_ctx_t;

static pin_ledger_ctx_t s_ledger_ctx;

/* ================================================================
 * Mise a jour de l'affichage
 * ================================================================ */

/**
 * Met a jour l'affichage des chiffres dans le header et le chiffre central.
 */
static void ledger_update_display(void)
{
    /* Mettre a jour les labels du header */
    for (int i = 0; i < UI_PIN_LENGTH; i++) {
        if (i < s_ledger_ctx.pos) {
            /* Chiffre deja saisi : afficher un dot */
            lv_label_set_text(s_ledger_ctx.digit_labels[i],
                              LV_SYMBOL_BULLET);
            lv_obj_set_style_text_color(s_ledger_ctx.digit_labels[i],
                                         UI_COLOR_ACCENT, 0);
        } else if (i == s_ledger_ctx.pos) {
            /* Chiffre en cours : afficher la valeur en surbrillance */
            char buf[2] = {(char)('0' + s_ledger_ctx.pin[i]), '\0'};
            lv_label_set_text(s_ledger_ctx.digit_labels[i], buf);
            lv_obj_set_style_text_color(s_ledger_ctx.digit_labels[i],
                                         UI_COLOR_ACCENT, 0);
        } else {
            /* Chiffre pas encore atteint : tiret */
            lv_label_set_text(s_ledger_ctx.digit_labels[i], "-");
            lv_obj_set_style_text_color(s_ledger_ctx.digit_labels[i],
                                         UI_COLOR_TEXT_DIM, 0);
        }
    }

    /* Grand chiffre central */
    char buf[2] = {(char)('0' + s_ledger_ctx.pin[s_ledger_ctx.pos]), '\0'};
    lv_label_set_text(s_ledger_ctx.current_digit_label, buf);
}

/* ================================================================
 * Callbacks
 * ================================================================ */

/**
 * Zone haute : incrementer le chiffre courant.
 */
static void ledger_up_cb(lv_event_t *e)
{
    (void)e;
    if (s_ledger_ctx.pos >= UI_PIN_LENGTH) return;

    s_ledger_ctx.pin[s_ledger_ctx.pos] =
        (s_ledger_ctx.pin[s_ledger_ctx.pos] + 1) % 10;
    ledger_update_display();
}

/**
 * Zone basse : decrementer le chiffre courant.
 */
static void ledger_down_cb(lv_event_t *e)
{
    (void)e;
    if (s_ledger_ctx.pos >= UI_PIN_LENGTH) return;

    s_ledger_ctx.pin[s_ledger_ctx.pos] =
        (s_ledger_ctx.pin[s_ledger_ctx.pos] + 9) % 10;
    ledger_update_display();
}

/**
 * Zone centrale : valider le chiffre courant et passer au suivant.
 * Si tous les chiffres sont saisis, appeler le callback.
 */
static void ledger_confirm_cb(lv_event_t *e)
{
    (void)e;
    if (s_ledger_ctx.pos >= UI_PIN_LENGTH) return;

    s_ledger_ctx.pos++;

    if (s_ledger_ctx.pos == UI_PIN_LENGTH) {
        /* PIN complet */
        if (s_ledger_ctx.callback) {
            s_ledger_ctx.callback(s_ledger_ctx.pin,
                                   s_ledger_ctx.user_data);
        }
    } else {
        ledger_update_display();
    }
}

/* ================================================================
 * Creation du widget
 * ================================================================ */

lv_obj_t *ui_pin_ledger_create(lv_obj_t *parent, const char *title,
                                ui_pin_callback_t callback, void *user_data)
{
    /* Reinitialiser le contexte */
    memset(&s_ledger_ctx, 0, sizeof(s_ledger_ctx));
    s_ledger_ctx.callback  = callback;
    s_ledger_ctx.user_data = user_data;

    /* Overlay plein ecran paysage (320x172) */
    lv_obj_t *overlay = lv_obj_create(parent ? parent : lv_screen_active());
    lv_obj_set_size(overlay, LV_PCT(100), LV_PCT(100));
    lv_obj_add_style(overlay, &ui_style_screen, 0);
    lv_obj_set_style_pad_all(overlay, 4, 0);
    lv_obj_set_flex_flow(overlay, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(overlay,
                          LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(overlay, 4, 0);

    /* Titre */
    lv_obj_t *lbl_title = lv_label_create(overlay);
    lv_obj_add_style(lbl_title, &ui_style_title, 0);
    lv_label_set_text(lbl_title, title);

    /* Indicateurs des 4 chiffres en ligne */
    lv_obj_t *digit_row = lv_obj_create(overlay);
    lv_obj_set_size(digit_row, LV_PCT(60), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(digit_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(digit_row,
                          LV_FLEX_ALIGN_SPACE_EVENLY,
                          LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_bg_opa(digit_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(digit_row, 0, 0);
    lv_obj_set_style_pad_all(digit_row, 0, 0);

    for (int i = 0; i < UI_PIN_LENGTH; i++) {
        lv_obj_t *lbl = lv_label_create(digit_row);
        lv_obj_set_style_text_font(lbl, ui_theme_font_title(), 0);
        lv_label_set_text(lbl, "-");
        lv_obj_set_style_text_color(lbl, UI_COLOR_TEXT_DIM, 0);
        s_ledger_ctx.digit_labels[i] = lbl;
    }

    /* Zone centrale : [▲]  [chiffre]  [▼] en ligne horizontale */
    lv_obj_t *ctrl_row = lv_obj_create(overlay);
    lv_obj_set_size(ctrl_row, LV_PCT(80), 56);
    lv_obj_set_flex_flow(ctrl_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(ctrl_row,
                          LV_FLEX_ALIGN_SPACE_EVENLY,
                          LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_bg_opa(ctrl_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(ctrl_row, 0, 0);
    lv_obj_set_style_pad_all(ctrl_row, 0, 0);

    /*
     * [F-UI-013] Bouton DECREMENT (flèche gauche). Anciennement nommé
     * `btn_down` avec commentaire "Bouton UP" — confusion résolue :
     * la flèche gauche décrémente le chiffre courant.
     */
    lv_obj_t *btn_decrement = lv_button_create(ctrl_row);
    lv_obj_set_size(btn_decrement, 70, 50);
    lv_obj_add_style(btn_decrement, &ui_style_btn, 0);
    lv_obj_add_event_cb(btn_decrement, ledger_down_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lbl_decrement = lv_label_create(btn_decrement);
    lv_label_set_text(lbl_decrement, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_font(lbl_decrement, ui_theme_font_amount(), 0);
    lv_obj_center(lbl_decrement);

    /* Grand chiffre central */
    s_ledger_ctx.current_digit_label = lv_label_create(ctrl_row);
    lv_obj_set_style_text_font(s_ledger_ctx.current_digit_label,
                                ui_theme_font_amount(), 0);
    lv_obj_set_style_text_color(s_ledger_ctx.current_digit_label,
                                 UI_COLOR_ACCENT, 0);
    lv_label_set_text(s_ledger_ctx.current_digit_label, "0");

    /*
     * [F-UI-013] Bouton INCREMENT (flèche droite). Anciennement nommé
     * `btn_up` avec commentaire "Bouton DOWN" — confusion résolue.
     */
    lv_obj_t *btn_increment = lv_button_create(ctrl_row);
    lv_obj_set_size(btn_increment, 70, 50);
    lv_obj_add_style(btn_increment, &ui_style_btn, 0);
    lv_obj_add_event_cb(btn_increment, ledger_up_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lbl_increment = lv_label_create(btn_increment);
    lv_label_set_text(lbl_increment, LV_SYMBOL_RIGHT);
    lv_obj_set_style_text_font(lbl_increment, ui_theme_font_amount(), 0);
    lv_obj_center(lbl_increment);

    /* Bouton de validation en bas */
    lv_obj_t *btn_ok = lv_button_create(overlay);
    lv_obj_set_size(btn_ok, 140, 36);
    lv_obj_add_style(btn_ok, &ui_style_btn, 0);
    lv_obj_set_style_bg_color(btn_ok, UI_COLOR_ACCENT, 0);
    lv_obj_add_event_cb(btn_ok, ledger_confirm_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lbl_ok = lv_label_create(btn_ok);
    lv_label_set_text(lbl_ok, "Valider " LV_SYMBOL_OK);
    lv_obj_center(lbl_ok);

    /* Initialiser l'affichage */
    ledger_update_display();

    return overlay;
}
