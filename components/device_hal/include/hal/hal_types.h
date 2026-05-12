/**
 * @file hal_types.h
 * @brief Types portables pour la couche d'abstraction matérielle (HAL).
 *
 * Ce fichier définit les types communs utilisés par toutes les interfaces HAL.
 * Il n'inclut AUCUN header spécifique à une plateforme (pas de esp_err.h, etc.)
 * afin de garantir la portabilité sur ESP32, STM32, ou tout autre matériel.
 */

#ifndef HAL_TYPES_H
#define HAL_TYPES_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/**
 * Code d'erreur portable pour la HAL.
 *
 * Convention : 0 = succès, valeurs négatives = erreurs.
 * Permet un test simple : if (err) { ... erreur ... }
 *
 * Les implémentations spécifiques (ESP32, STM32, mock) traduisent
 * leurs codes d'erreur natifs vers ces valeurs à la frontière.
 */
typedef enum {
    HAL_OK            =  0,  /* Opération réussie */
    HAL_FAIL          = -1,  /* Erreur générique */
    HAL_ERR_NOT_FOUND = -2,  /* Clé ou ressource introuvable */
    HAL_ERR_NO_MEM    = -3,  /* Mémoire insuffisante */
    HAL_ERR_INVALID   = -4,  /* Paramètre invalide */
    HAL_ERR_TIMEOUT   = -5,  /* Délai d'attente dépassé */
    HAL_ERR_BUSY      = -6,  /* Ressource occupée */
    HAL_ERR_IO        = -7,  /* Erreur d'entrée/sortie */
} hal_err_t;

#endif /* HAL_TYPES_H */
