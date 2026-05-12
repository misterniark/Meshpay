/**
 * @file ui_pin.h
 * @brief API commune pour la saisie de code PIN.
 *
 * Deux implementations existent :
 * - ui_pin_numpad.c : pave numerique 3x4 pour le CYD (320x240)
 * - ui_pin_ledger.c : style Ledger a 2 zones tactiles pour le
 *   Waveshare (172x320) dont l'ecran est trop petit pour un clavier
 *
 * L'implementation est choisie automatiquement selon la resolution.
 */

#ifndef UI_PIN_H
#define UI_PIN_H

#include "hal/hal_storage.h"
#include "lvgl.h"
#include <stdint.h>
#include <stdbool.h>

/** Longueur du code PIN */
#define UI_PIN_LENGTH 4

/** Namespace NVS pour le stockage PIN */
#define UI_PIN_NVS_NS "pin"

/* ================================================================
 * API de gestion du PIN (hash, stockage, verification)
 * ================================================================ */

/**
 * Resultat de la verification d'un PIN.
 */
typedef enum {
    UI_PIN_OK,              /* PIN correct */
    UI_PIN_WRONG,           /* PIN incorrect */
    UI_PIN_BLOCKED,         /* Trop de tentatives, device bloque */
    UI_PIN_COOLDOWN,        /* En attente (delai anti brute-force) */
    UI_PIN_WEAK,            /* PIN trop faible (0000, 1234, etc.) */
} ui_pin_result_t;

/**
 * Verifie si un PIN est deja configure dans le NVS.
 *
 * @param storage HAL storage
 * @return true si un hash PIN existe dans le NVS
 */
bool ui_pin_is_configured(hal_storage_t *storage);

/**
 * Enregistre un nouveau PIN dans le NVS (hash SHA-256).
 *
 * Verifie d'abord que le PIN n'est pas dans la blacklist
 * des codes faibles (0000, 1234, 5678, etc.).
 *
 * @param pin     Tableau de 4 chiffres (0-9)
 * @param storage HAL storage
 * @return UI_PIN_OK si enregistre, UI_PIN_WEAK si blackliste
 */
ui_pin_result_t ui_pin_register(const uint8_t pin[UI_PIN_LENGTH],
                                hal_storage_t *storage);

/**
 * Verifie un PIN saisi contre le hash stocke en NVS.
 *
 * Gere le delai anti brute-force et le compteur d'echecs.
 *
 * @param pin     Tableau de 4 chiffres (0-9)
 * @param storage HAL storage
 * @return UI_PIN_OK, UI_PIN_WRONG, UI_PIN_BLOCKED ou UI_PIN_COOLDOWN
 */
ui_pin_result_t ui_pin_verify(const uint8_t pin[UI_PIN_LENGTH],
                              hal_storage_t *storage);

/**
 * Retourne le delai restant avant la prochaine tentative (en secondes).
 * 0 si aucune restriction active.
 *
 * @param storage HAL storage
 * @return Secondes restantes avant prochaine tentative autorisee
 */
uint32_t ui_pin_cooldown_remaining(hal_storage_t *storage);

/**
 * Verifie si un PIN est dans la blacklist des codes faibles.
 *
 * @param pin Tableau de 4 chiffres
 * @return true si le PIN est interdit
 */
bool ui_pin_is_weak(const uint8_t pin[UI_PIN_LENGTH]);

/**
 * Callback appele quand un PIN est saisi completement.
 *
 * @param pin     Le PIN saisi (tableau de 4 chiffres, chaque valeur 0-9)
 * @param user_data Donnee utilisateur passee a ui_pin_show()
 */
typedef void (*ui_pin_callback_t)(const uint8_t pin[UI_PIN_LENGTH], void *user_data);

/**
 * Afficher l'ecran de saisie PIN.
 *
 * Cree un overlay modal LVGL pour la saisie du PIN.
 * Appelle le callback quand les 4 chiffres sont saisis.
 *
 * @param parent    Objet parent LVGL (ou NULL pour fullscreen)
 * @param title     Titre affiche (ex: "Entrez votre PIN", "Nouveau PIN")
 * @param callback  Fonction appelee avec le PIN saisi
 * @param user_data Donnee passee au callback
 * @param is_small  true pour utiliser le mode Ledger (Waveshare)
 * @return Objet LVGL cree (pour pouvoir le detruire si necessaire)
 */
lv_obj_t *ui_pin_show(lv_obj_t *parent, const char *title,
                       ui_pin_callback_t callback, void *user_data,
                       bool is_small);

/**
 * Fermer l'ecran de saisie PIN actif.
 */
void ui_pin_close(void);

/* ================================================================
 * Fonctions internes (une par implementation)
 * ================================================================ */

/** Cree le widget PIN numpad (grand ecran) */
lv_obj_t *ui_pin_numpad_create(lv_obj_t *parent, const char *title,
                                ui_pin_callback_t callback, void *user_data);

/** Cree le widget PIN Ledger-style (petit ecran) */
lv_obj_t *ui_pin_ledger_create(lv_obj_t *parent, const char *title,
                                ui_pin_callback_t callback, void *user_data);

#endif /* UI_PIN_H */
