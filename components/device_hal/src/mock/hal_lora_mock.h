/**
 * @file hal_lora_mock.h
 * @brief Mock LoRa pour tests — ring buffer loopback.
 *
 * Les paquets envoyés via send() sont stockés dans un buffer interne.
 * Appeler hal_lora_mock_pump() délivre le prochain paquet au callback RX,
 * simulant la réception asynchrone.
 */

#ifndef HAL_LORA_MOCK_H
#define HAL_LORA_MOCK_H

#include "hal/hal_lora.h"

/**
 * Créer une instance mock LoRa.
 *
 * @param lora [out] Vtable à remplir
 * @return HAL_OK
 */
hal_err_t hal_lora_mock_create(hal_lora_t *lora);

/**
 * Réinitialiser le mock (vider le buffer, supprimer le callback).
 *
 * @param lora Instance mock
 */
void hal_lora_mock_reset(hal_lora_t *lora);

/**
 * Simuler la réception du prochain paquet dans le buffer.
 * Appelle le callback RX enregistré avec les données du paquet.
 *
 * @param lora Instance mock
 * @return HAL_OK si un paquet a été délivré, HAL_ERR_NOT_FOUND si vide
 */
hal_err_t hal_lora_mock_pump(hal_lora_t *lora);

/**
 * Nombre de paquets en attente dans le buffer.
 *
 * @param lora Instance mock
 * @return Nombre de paquets
 */
uint32_t hal_lora_mock_pending_count(const hal_lora_t *lora);

#endif /* HAL_LORA_MOCK_H */
