/**
 * @file app_init/nvs_init.h
 * @brief Initialisation NVS (chiffree ou claire selon CONFIG_NVS_ENCRYPTION).
 *
 * Pattern facade : un seul header, deux impls selectionnees par CMake :
 *   - `nvs_init_secure.c` : impl AES-XTS (compile si CONFIG_NVS_ENCRYPTION=y)
 *   - `nvs_init_plain.c`  : impl standard (compile sinon)
 *
 * Le code applicatif n'a plus aucun `#if defined(CONFIG_NVS_ENCRYPTION)`
 * en main.c.
 */

#ifndef MESHPAY_APP_INIT_NVS_INIT_H
#define MESHPAY_APP_INIT_NVS_INIT_H

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialise NVS (avec chiffrement si configure).
 *
 * Si l'init NVS necessite un effacement (no_free_pages ou new_version),
 * applique aussi la mitigation [C11] : verrouille le compteur PIN pour
 * empecher un attaquant de contourner le brute-force en effacant le NVS.
 *
 * @param[out] out_encrypted Indique si NVS est en mode chiffre.
 * @return ESP_OK en cas de succes.
 */
esp_err_t nvs_init_storage(bool *out_encrypted);

#ifdef __cplusplus
}
#endif

#endif /* MESHPAY_APP_INIT_NVS_INIT_H */
