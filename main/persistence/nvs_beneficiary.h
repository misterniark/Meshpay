/**
 * @file persistence/nvs_beneficiary.h
 * @brief Configuration beneficiaire pour l'auto-forward (NVS).
 *
 * Quand `s_forward_interval_min > 0`, le device transfere periodiquement
 * son solde a `s_beneficiary_key`. Configuration recue via LoRa
 * (SET_BENEFICIARY) puis persistee.
 */

#ifndef MESHPAY_PERSISTENCE_NVS_BENEFICIARY_H
#define MESHPAY_PERSISTENCE_NVS_BENEFICIARY_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Charge `s_beneficiary_key` + `s_forward_interval_min` depuis NVS
 *        si la clef beneficiaire est presente. Sinon laisse les valeurs
 *        par defaut (zero/inactif) deja initialisees dans app_state.c.
 *
 * @return ESP_OK toujours (l'absence d'entree n'est pas une erreur).
 */
esp_err_t nvs_beneficiary_load(void);

#ifdef __cplusplus
}
#endif

#endif /* MESHPAY_PERSISTENCE_NVS_BENEFICIARY_H */
