/**
 * @file ui_manager.h
 * @brief Gestion des ecrans et navigation.
 *
 * Fournit une API simple pour naviguer entre les ecrans :
 * - ui_manager_init() : initialise la table des handlers
 * - ui_manager_show() : affiche un ecran (empile le precedent)
 * - ui_manager_back() : retourne a l'ecran precedent
 * - ui_manager_update() : rafraichit l'ecran courant
 */

#ifndef UI_MANAGER_H
#define UI_MANAGER_H

#include <stdbool.h>

#include "ui/ui_screens.h"
#include "ui/ui_state.h"

/** Profondeur max de la pile de navigation */
#define UI_NAV_STACK_DEPTH  8

/**
 * Initialiser le gestionnaire d'ecrans.
 *
 * Enregistre tous les handlers d'ecrans dans la table interne.
 * Doit etre appele une seule fois depuis ui_task apres lv_init().
 *
 * @param ctx Contexte UI partage
 */
void ui_manager_init(ui_ctx_t *ctx);

/**
 * Afficher un ecran.
 *
 * Detruit l'ecran courant et cree le nouveau.
 * L'ecran precedent est empile pour permettre un retour.
 *
 * Echoue (retourne false sans modifier l'etat) si :
 *   - screen_id est hors plage ou son handler/create est absent ;
 *   - la pile de navigation est saturee (UI_NAV_STACK_DEPTH atteint).
 *
 * @param screen_id Identifiant de l'ecran a afficher
 * @return true si l'ecran a ete affiche, false en cas d'echec.
 */
bool ui_manager_show(ui_screen_id_t screen_id);

/**
 * Retourner a l'ecran precedent dans la pile.
 *
 * Si la pile est vide, affiche l'ecran HOME.
 * Si le handler de l'ecran cible est absent, retombe sur HOME ;
 * si meme HOME est absent, retourne false sans modifier l'etat.
 *
 * @return true si un ecran a ete affiche, false en cas d'echec total.
 */
bool ui_manager_back(void);

/**
 * Rafraichir les donnees de l'ecran courant.
 *
 * Appele periodiquement depuis la boucle ui_task
 * pour mettre a jour les valeurs affichees (solde, etc.).
 */
void ui_manager_update(void);

/**
 * Obtenir l'identifiant de l'ecran courant.
 */
ui_screen_id_t ui_manager_current(void);

/**
 * Profondeur actuelle de la pile de navigation.
 *
 * 0 si la pile est vide (on est sur l'ecran initial sans historique),
 * jusqu'a UI_NAV_STACK_DEPTH.
 *
 * Expose principalement pour l'instrumentation et les tests : permet
 * de detecter une derive (saturation, fuite de chemin de retour).
 */
int ui_manager_nav_depth(void);

/**
 * Remplacer un handler d'ecran.
 *
 * API d'instrumentation pour test_app : permet de tester la navigation
 * avec des handlers fictifs sans linker des doubles des vrais ecrans.
 */
bool ui_manager_set_handler_for_test(ui_screen_id_t screen_id,
                                     ui_screen_handler_t *handler);

#endif /* UI_MANAGER_H */
