/**
 * @file ui_screens.h
 * @brief Enumeration de tous les ecrans et interface commune.
 *
 * Chaque ecran implemente un handler (create, update, destroy).
 * Le ui_manager utilise ces handlers pour naviguer entre les ecrans.
 */

#ifndef UI_SCREENS_H
#define UI_SCREENS_H

#include "lvgl.h"

/* Forward declaration */
typedef struct ui_ctx_s ui_ctx_t;

/**
 * Identifiants des ecrans.
 *
 * Les ecrans 1.x sont accessibles a tous les devices (clients + maitres).
 * Les ecrans 2.x sont reserves aux devices maitres.
 */
typedef enum {
    /* Ecrans clients (tous les devices) */
    UI_SCREEN_SETUP,       /* 1.0 — Premier boot : PIN + alias */
    UI_SCREEN_HOME,        /* 1.1 — Accueil / solde */
    UI_SCREEN_PAY,         /* 1.2 — Payer (4 etapes) */
    UI_SCREEN_HISTORY,     /* 1.3 — Historique transactions */
    UI_SCREEN_SETTINGS,    /* 1.4 — Parametres */
    UI_SCREEN_RECEIVE,     /* 1.5 — Reception paiement */
    UI_SCREEN_BROADCAST,   /* 1.6 — Notification broadcast */

    /* Ecrans maitre uniquement */
    UI_SCREEN_ADMIN,       /* 2.1 — Menu admin */
    UI_SCREEN_MINT,        /* 2.2 — Creer des credits */
    UI_SCREEN_MESSAGE,     /* 2.3 — Envoyer un broadcast texte */
    UI_SCREEN_SCAN,        /* 2.4 — Scanner les devices */
    UI_SCREEN_RENAME,      /* 2.5 — Renommer un device distant */
    UI_SCREEN_FORWARD,     /* 2.6 — Configurer auto-forward */

    UI_SCREEN_COUNT        /* Nombre total d'ecrans */
} ui_screen_id_t;

/**
 * Interface qu'un ecran doit implementer.
 *
 * - create() : construit l'arbre LVGL de l'ecran et retourne l'objet racine.
 * - update() : rafraichit les donnees affichees (appelé depuis ui_task).
 * - destroy() : libere les ressources allouees par create().
 */
typedef struct {
    lv_obj_t * (*create)(ui_ctx_t *ctx);
    void       (*update)(ui_ctx_t *ctx);
    void       (*destroy)(void);
} ui_screen_handler_t;

#endif /* UI_SCREENS_H */
