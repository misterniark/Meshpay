/**
 * @file crypto_hash.h
 * @brief Fonctions de hachage SHA-256.
 *
 * Utilisé pour calculer l'identifiant unique de chaque transaction
 * dans le DAG. Le hash est calculé sur le contenu sérialisé de la
 * transaction (tous les champs sauf id et signature).
 */

#ifndef CRYPTO_HASH_H
#define CRYPTO_HASH_H

#include "crypto/crypto_types.h"
#include "esp_err.h"
#include <stddef.h>

/**
 * @brief Calcule le hash SHA-256 d'un buffer.
 *
 * @param[in]  data     Données à hasher
 * @param[in]  data_len Taille des données en octets
 * @param[out] hash     Hash SHA-256 résultant (32 octets)
 * @return ESP_OK en cas de succès, ESP_FAIL en cas d'erreur
 */
esp_err_t crypto_hash_sha256(const uint8_t *data, size_t data_len, hash_t *hash);

#endif /* CRYPTO_HASH_H */
