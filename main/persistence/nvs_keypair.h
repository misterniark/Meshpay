/**
 * @file persistence/nvs_keypair.h
 * @brief Charge ou genere le keypair Ed25519 du device.
 *
 * Au premier boot : `crypto_generate_keypair()` + sauvegarde NVS.
 * Aux boots suivants : chargement depuis NVS (privkey + pubkey).
 *
 * Le keypair est ecrit dans `s_keypair` (app_state).
 */

#ifndef MESHPAY_PERSISTENCE_NVS_KEYPAIR_H
#define MESHPAY_PERSISTENCE_NVS_KEYPAIR_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @return ESP_OK en cas de succes (charge ou genere).
 */
esp_err_t nvs_keypair_load_or_generate(void);

#ifdef __cplusplus
}
#endif

#endif /* MESHPAY_PERSISTENCE_NVS_KEYPAIR_H */
