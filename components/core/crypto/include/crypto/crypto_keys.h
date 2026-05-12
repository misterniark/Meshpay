/**
 * @file crypto_keys.h
 * @brief Gestion des paires de clés Ed25519.
 *
 * Fournit les fonctions pour générer, exporter et importer
 * des paires de clés Ed25519. La génération utilise le PRNG
 * de mbedtls (ou un PRNG injecté pour les tests).
 */

#ifndef CRYPTO_KEYS_H
#define CRYPTO_KEYS_H

#include "crypto/crypto_types.h"
#include "esp_err.h"

/**
 * @brief Génère une nouvelle paire de clés Ed25519.
 *
 * Utilise le générateur aléatoire matériel de l'ESP32 via mbedtls.
 * La clé privée contient la seed (32 octets) concaténée avec
 * la clé publique (32 octets) pour un total de 64 octets.
 *
 * @param[out] keypair Paire de clés générée
 * @return ESP_OK en cas de succès, ESP_FAIL en cas d'erreur
 */
esp_err_t crypto_generate_keypair(keypair_t *keypair);

/**
 * @brief Exporte la clé publique depuis une paire de clés.
 *
 * Copie la partie publique de la paire de clés dans la structure
 * de sortie. Utile pour partager l'identité du device sans
 * exposer la clé privée.
 *
 * @param[in]  keypair Paire de clés source
 * @param[out] pubkey  Clé publique extraite
 * @return ESP_OK en cas de succès, ESP_ERR_INVALID_ARG si un pointeur est NULL
 */
esp_err_t crypto_export_public_key(const keypair_t *keypair, public_key_t *pubkey);

/**
 * @brief Importe une paire de clés depuis des buffers bruts.
 *
 * Reconstruit une structure keypair_t à partir d'une clé privée
 * (64 octets) et d'une clé publique (32 octets). Vérifie la
 * cohérence entre les deux (la clé publique dans private_key[32..63]
 * doit correspondre à pubkey).
 *
 * @param[out] keypair    Paire de clés reconstruite
 * @param[in]  privkey    Clé privée brute (64 octets)
 * @param[in]  pubkey     Clé publique brute (32 octets)
 * @return ESP_OK si cohérent, ESP_ERR_INVALID_ARG si incohérent ou NULL
 */
esp_err_t crypto_import_keypair(keypair_t *keypair, const uint8_t *privkey,
                                const uint8_t *pubkey);

#endif /* CRYPTO_KEYS_H */
