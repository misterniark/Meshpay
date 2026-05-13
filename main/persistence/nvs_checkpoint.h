/**
 * @file persistence/nvs_checkpoint.h
 * @brief Persistance NVS du checkpoint wallet.
 *
 * Implementations des signatures `checkpoint_save_fn` / `checkpoint_load_fn`
 * (composant wallet). Gere la migration des anciens formats (avant
 * `last_melt_timestamp`).
 *
 * Pour brancher un backend mock (memoire, SPIFFS, SD), il suffit de
 * reassigner `s_checkpoint_save` / `s_checkpoint_load` (declares dans
 * app_state.h) avant le boot.
 */

#ifndef MESHPAY_PERSISTENCE_NVS_CHECKPOINT_H
#define MESHPAY_PERSISTENCE_NVS_CHECKPOINT_H

#include "esp_err.h"
#include "wallet/wallet_checkpoint.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Charge le checkpoint depuis NVS.
 * @return ESP_OK si charge, ESP_ERR_NOT_FOUND si aucun, ESP_FAIL sinon.
 */
esp_err_t nvs_checkpoint_load(checkpoint_t *checkpoint, void *ctx);

/**
 * @brief Sauvegarde le checkpoint dans NVS.
 */
esp_err_t nvs_checkpoint_save(const checkpoint_t *checkpoint, void *ctx);

#ifdef __cplusplus
}
#endif

#endif /* MESHPAY_PERSISTENCE_NVS_CHECKPOINT_H */
