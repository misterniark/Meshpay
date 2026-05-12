/**
 * @file espnow_hal_mock.h
 * @brief Mock ESP-NOW pour tests unitaires.
 *
 * Ring buffer de paquets envoyés + injection de paquets reçus.
 * Permet de tester le protocole ESP-NOW sans Wi-Fi ni matériel.
 */

#ifndef ESPNOW_HAL_MOCK_H
#define ESPNOW_HAL_MOCK_H

#include "comm/espnow_hal.h"

/**
 * Créer une instance mock ESP-NOW.
 *
 * @param hal [out] Vtable à remplir
 * @return HAL_OK
 */
hal_err_t espnow_hal_mock_create(espnow_hal_t *hal);

/** Réinitialiser le mock (vider les buffers, supprimer le callback). */
void espnow_hal_mock_reset(espnow_hal_t *hal);

/**
 * Injecter un paquet comme s'il venait d'un autre device.
 * Appelle directement le callback RX enregistré.
 *
 * @param hal     Instance mock
 * @param src_mac Adresse MAC simulée de l'émetteur
 * @param data    Données du paquet
 * @param len     Taille des données
 * @return HAL_OK, HAL_ERR_INVALID si pas de callback
 */
hal_err_t espnow_hal_mock_inject(espnow_hal_t *hal,
                                  const uint8_t *src_mac,
                                  const uint8_t *data, size_t len);

/**
 * Récupérer le dernier paquet envoyé via send() ou broadcast().
 *
 * @param hal      Instance mock
 * @param dest_mac [out] MAC destination (6 octets)
 * @param data     [out] Buffer pour les données
 * @param len      [in/out] En entrée : taille buffer. En sortie : taille données.
 * @return HAL_OK si un paquet est disponible, HAL_ERR_NOT_FOUND sinon
 */
hal_err_t espnow_hal_mock_get_last_sent(const espnow_hal_t *hal,
                                         uint8_t *dest_mac,
                                         uint8_t *data, size_t *len);

/** Nombre de paquets envoyés depuis le dernier reset. */
uint32_t espnow_hal_mock_sent_count(const espnow_hal_t *hal);

#endif /* ESPNOW_HAL_MOCK_H */
