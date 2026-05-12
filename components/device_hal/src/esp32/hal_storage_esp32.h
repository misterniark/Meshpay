/**
 * @file hal_storage_esp32.h
 * @brief Factory pour l'implémentation NVS du stockage sur ESP32.
 *
 * Utilise la partition NVS (Non-Volatile Storage) d'ESP-IDF pour
 * persister les données. Supporte le chiffrement NVS si la flash
 * encryption est activée.
 *
 * Thread-safe : NVS est protégé par mutex en interne dans ESP-IDF.
 */

#ifndef HAL_STORAGE_ESP32_H
#define HAL_STORAGE_ESP32_H

#include "hal/hal_storage.h"

/**
 * Créer une instance de stockage basée sur NVS ESP-IDF.
 *
 * Initialise la partition NVS (nvs_flash_init) et remplit la vtable
 * avec les fonctions qui encapsulent l'API NVS.
 *
 * @param storage [out] Vtable à remplir
 * @return HAL_OK en cas de succès, HAL_FAIL si l'init NVS échoue
 */
hal_err_t hal_storage_esp32_create(hal_storage_t *storage);

#endif /* HAL_STORAGE_ESP32_H */
