/**
 * @file ui_theme.h
 * @brief Theme visuel et styles LVGL partages.
 *
 * Definit la palette de couleurs, les styles de boutons, cartes,
 * textes, et les polices adaptees a chaque taille d'ecran.
 *
 * Les styles sont crees une seule fois par ui_theme_init() et
 * reutilises par tous les ecrans.
 */

#ifndef UI_THEME_H
#define UI_THEME_H

#include "lvgl.h"

/* ================================================================
 * Palette de couleurs
 * ================================================================ */

#define UI_COLOR_BG          lv_color_hex(0x1A1A2E) /* Fond sombre */
#define UI_COLOR_CARD        lv_color_hex(0x16213E) /* Fond des cartes/sections */
#define UI_COLOR_BTN         lv_color_hex(0x0F3460) /* Boutons principaux */
#define UI_COLOR_BTN_PRESSED lv_color_hex(0x1A5276) /* Bouton presse */
#define UI_COLOR_ACCENT      lv_color_hex(0xE94560) /* Accent (montants, alertes) */
#define UI_COLOR_SUCCESS     lv_color_hex(0x27AE60) /* Succes (paiement confirme) */
#define UI_COLOR_WARNING     lv_color_hex(0xF39C12) /* Avertissement */
#define UI_COLOR_TEXT        lv_color_hex(0xECECEC) /* Texte principal */
#define UI_COLOR_TEXT_DIM    lv_color_hex(0x888888) /* Texte secondaire */

/* ================================================================
 * Styles partages
 * ================================================================ */

/** Style de base pour le fond d'ecran */
extern lv_style_t ui_style_screen;

/** Style pour les boutons d'action */
extern lv_style_t ui_style_btn;

/** Style pour les cartes / panneaux */
extern lv_style_t ui_style_card;

/** Style pour le texte de titre */
extern lv_style_t ui_style_title;

/** Style pour le texte normal */
extern lv_style_t ui_style_text;

/** Style pour les montants / valeurs importantes */
extern lv_style_t ui_style_amount;

/** Style pour le header (barre superieure) */
extern lv_style_t ui_style_header;

/* ================================================================
 * API
 * ================================================================ */

/**
 * Initialiser le theme et creer les styles partages.
 *
 * Adapte les polices selon la taille d'ecran :
 * - Grand ecran (CYD) : Montserrat 20 normal, 28 montants
 * - Petit ecran (Waveshare) : Montserrat 14 normal, 20 montants
 *
 * @param is_small_screen true pour le Waveshare 172x320
 */
void ui_theme_init(bool is_small_screen);

/**
 * Obtenir la police adaptee au texte normal.
 */
const lv_font_t *ui_theme_font_normal(void);

/**
 * Obtenir la police adaptee aux titres.
 */
const lv_font_t *ui_theme_font_title(void);

/**
 * Obtenir la police adaptee aux montants.
 */
const lv_font_t *ui_theme_font_amount(void);

#endif /* UI_THEME_H */
