/**
 * @file ui_theme.c
 * @brief Implementation du theme visuel LVGL.
 *
 * Cree les styles partages utilises par tous les ecrans.
 * Les polices sont adaptees selon la taille de l'ecran :
 * - CYD (320x240) : polices plus grandes
 * - Waveshare (172x320) : polices compactes
 */

#include "ui/ui_theme.h"

/* ================================================================
 * Styles partages (definis dans le header comme extern)
 * ================================================================ */

lv_style_t ui_style_screen;
lv_style_t ui_style_btn;
lv_style_t ui_style_card;
lv_style_t ui_style_title;
lv_style_t ui_style_text;
lv_style_t ui_style_amount;
lv_style_t ui_style_header;

/* Police selectionnee selon la taille d'ecran */
static const lv_font_t *s_font_normal = NULL;
static const lv_font_t *s_font_title  = NULL;
static const lv_font_t *s_font_amount = NULL;

/* ================================================================
 * API
 * ================================================================ */

void ui_theme_init(bool is_small_screen)
{
    /* Selectionner les polices selon la taille d'ecran */
    if (is_small_screen) {
        s_font_normal = &lv_font_montserrat_14;
        s_font_title  = &lv_font_montserrat_14;
        s_font_amount = &lv_font_montserrat_20;
    } else {
        s_font_normal = &lv_font_montserrat_14;
        s_font_title  = &lv_font_montserrat_20;
        s_font_amount = &lv_font_montserrat_28;
    }

    /* --- Style ecran (fond) --- */
    lv_style_init(&ui_style_screen);
    lv_style_set_bg_color(&ui_style_screen, UI_COLOR_BG);
    lv_style_set_bg_opa(&ui_style_screen, LV_OPA_COVER);

    /* --- Style bouton --- */
    lv_style_init(&ui_style_btn);
    lv_style_set_bg_color(&ui_style_btn, UI_COLOR_BTN);
    lv_style_set_bg_opa(&ui_style_btn, LV_OPA_COVER);
    lv_style_set_text_color(&ui_style_btn, UI_COLOR_TEXT);
    lv_style_set_text_font(&ui_style_btn, s_font_normal);
    lv_style_set_radius(&ui_style_btn, 8);
    lv_style_set_pad_all(&ui_style_btn, is_small_screen ? 6 : 10);

    /* --- Style carte --- */
    lv_style_init(&ui_style_card);
    lv_style_set_bg_color(&ui_style_card, UI_COLOR_CARD);
    lv_style_set_bg_opa(&ui_style_card, LV_OPA_COVER);
    lv_style_set_radius(&ui_style_card, 12);
    lv_style_set_pad_all(&ui_style_card, is_small_screen ? 8 : 12);
    lv_style_set_border_width(&ui_style_card, 0);

    /* --- Style titre --- */
    lv_style_init(&ui_style_title);
    lv_style_set_text_color(&ui_style_title, UI_COLOR_TEXT);
    lv_style_set_text_font(&ui_style_title, s_font_title);

    /* --- Style texte normal --- */
    lv_style_init(&ui_style_text);
    lv_style_set_text_color(&ui_style_text, UI_COLOR_TEXT);
    lv_style_set_text_font(&ui_style_text, s_font_normal);

    /* --- Style montant --- */
    lv_style_init(&ui_style_amount);
    lv_style_set_text_color(&ui_style_amount, UI_COLOR_ACCENT);
    lv_style_set_text_font(&ui_style_amount, s_font_amount);

    /* --- Style header --- */
    lv_style_init(&ui_style_header);
    lv_style_set_bg_color(&ui_style_header, UI_COLOR_CARD);
    lv_style_set_bg_opa(&ui_style_header, LV_OPA_COVER);
    lv_style_set_text_color(&ui_style_header, UI_COLOR_TEXT);
    lv_style_set_text_font(&ui_style_header, s_font_normal);
    lv_style_set_pad_all(&ui_style_header, is_small_screen ? 4 : 8);
    lv_style_set_radius(&ui_style_header, 0);
}

const lv_font_t *ui_theme_font_normal(void)
{
    return s_font_normal;
}

const lv_font_t *ui_theme_font_title(void)
{
    return s_font_title;
}

const lv_font_t *ui_theme_font_amount(void)
{
    return s_font_amount;
}
