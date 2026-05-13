/**
 * @file balance.h
 * @brief Calcul du solde du proprietaire (checkpoint + DAG).
 *
 * compute_owner_balance lit le solde consolide du checkpoint et y
 * applique les TX post-checkpoint. Doit etre appele sous s_state_mutex.
 *
 * ui_get_owner_balance est le callback expose a l'UI (appele par le ctx
 * UI qui detient deja le mutex au moment du refresh ecran).
 */

#ifndef MESHPAY_BALANCE_H
#define MESHPAY_BALANCE_H

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t compute_owner_balance(uint32_t *out_balance);
uint32_t  ui_get_owner_balance(void);

#ifdef __cplusplus
}
#endif

#endif /* MESHPAY_BALANCE_H */
