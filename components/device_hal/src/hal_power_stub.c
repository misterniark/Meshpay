/**
 * @file hal_power_stub.c
 * @brief Implementation stub de hal_power — renvoie toujours USB.
 *
 * Le hardware de detection de la source d'alimentation n'existe pas
 * encore. Cette impl est inoffensive : elle declare que le device est
 * toujours sur USB, ce qui maintient le firmware en mode pleine
 * puissance (la machine power_manager ne passe jamais en ECO sur USB).
 *
 * A remplacer par une vraie factory (lecture GPIO/ADC) quand la carte
 * batterie sera prete.
 */

#include "hal/hal_power.h"

/* Pas de contexte necessaire : la reponse est constante. */
static hal_power_source_t stub_get_source(void *ctx)
{
    (void)ctx;
    return POWER_SOURCE_USB;
}

hal_err_t hal_power_stub_create(hal_power_t *out)
{
    if (out == NULL) {
        return HAL_ERR_INVALID;
    }
    out->get_source = stub_get_source;
    out->ctx = NULL;
    return HAL_OK;
}
