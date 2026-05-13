/**
 * @file persistence/nvs_alias.h
 * @brief Alias lisible du device (persistance NVS).
 *
 * Format : "<Adjectif>-<Animal>" (ex: "Brave-Loup"). Au premier boot,
 * genere a partir des 2 derniers octets de la pubkey (deterministe par
 * device, ~8 bits d'entropie = 256 combinaisons distinguables localement).
 * Aux boots suivants, charge depuis NVS.
 *
 * Ecrit dans `s_device_alias` / `s_device_alias_len` (app_state).
 */

#ifndef MESHPAY_PERSISTENCE_NVS_ALIAS_H
#define MESHPAY_PERSISTENCE_NVS_ALIAS_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t nvs_alias_load_or_generate(void);

#ifdef __cplusplus
}
#endif

#endif /* MESHPAY_PERSISTENCE_NVS_ALIAS_H */
