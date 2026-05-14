/**
 * @file hal_power.h
 * @brief Interface abstraite de la source d'alimentation (HAL).
 *
 * Permet au firmware de savoir s'il tourne sur USB ou sur batterie,
 * sans connaitre le mecanisme materiel de detection (GPIO, ADC...).
 *
 * Une seule operation : get_source(). Chaque implementation fournit
 * une fonction factory qui remplit la vtable.
 *
 * Etat actuel : le hardware batterie n'existe pas encore. La seule impl
 * disponible est hal_power_stub.c qui renvoie toujours POWER_SOURCE_USB.
 * Une vraie impl lira un GPIO/ADC quand la carte batterie sera prete.
 *
 * Portabilite : ce header n'inclut aucun header specifique plateforme.
 */

#ifndef HAL_POWER_H
#define HAL_POWER_H

#include "hal/hal_types.h"

/**
 * Source d'alimentation du device.
 */
typedef enum {
    POWER_SOURCE_USB,      /* Alimente en USB / secteur */
    POWER_SOURCE_BATTERY,  /* Sur batterie */
    POWER_SOURCE_UNKNOWN,  /* Indetermine — a traiter comme BATTERY (prudent) */
} hal_power_source_t;

/**
 * Vtable de la source d'alimentation.
 */
typedef struct hal_power_s {
    /**
     * Retourne la source d'alimentation courante.
     * DOIT etre thread-safe.
     *
     * @param ctx Contexte opaque
     * @return La source d'alimentation detectee
     */
    hal_power_source_t (*get_source)(void *ctx);

    /** Contexte opaque passe a get_source(). */
    void *ctx;
} hal_power_t;

/**
 * @brief Factory de l'implementation stub (toujours POWER_SOURCE_USB).
 *
 * A remplacer par une vraie factory (GPIO/ADC) quand le hardware
 * batterie sera disponible.
 *
 * @param out Vtable a remplir
 * @return HAL_OK
 */
hal_err_t hal_power_stub_create(hal_power_t *out);

#endif /* HAL_POWER_H */
