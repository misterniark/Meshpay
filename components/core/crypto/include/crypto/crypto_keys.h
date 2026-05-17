/**
 * @file crypto_keys.h
 * @brief Gestion des paires de clés Ed25519.
 *
 * Fournit les fonctions pour générer, exporter et importer des paires
 * de clés Ed25519. Depuis le Lot E.2, l'implémentation utilise Monocypher
 * (vendoré) pour l'API `crypto_ed25519_*`, et
 * `esp_fill_random()` (TRNG matériel ESP32) pour la source d'entropie.
 */

#ifndef CRYPTO_KEYS_H
#define CRYPTO_KEYS_H

#include "crypto/crypto_types.h"
#include "esp_err.h"
#include <stddef.h>

/**
 * @brief Génère une nouvelle paire de clés Ed25519.
 *
 * Utilise `esp_fill_random()` (TRNG matériel ESP32) pour générer la seed,
 * puis dérive la paire (seed||public, public) via Monocypher
 * (`crypto_ed25519_key_pair`). La clé privée contient la seed (32 octets)
 * concaténée avec la clé publique (32 octets) pour un total de 64 octets.
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
 * (64 octets) et d'une clé publique (32 octets). Effectue un test
 * cryptographique sign+verify pour confirmer la cohérence des deux
 * clés (un attaquant ne peut pas forger une paire en collant un
 * pubkey arbitraire après une privkey).
 *
 * [F-CR-003] Les paramètres `privkey_len` et `pubkey_len` sont
 * obligatoires et vérifiés contre `CRYPTO_PRIVATE_KEY_SIZE` (64) et
 * `CRYPTO_PUBLIC_KEY_SIZE` (32). Sans cette vérification, un buffer
 * tronqué (NVS partiellement corrompu) provoquerait une lecture
 * out-of-bounds dans le `memcpy` interne.
 *
 * @param[out] keypair      Paire de clés reconstruite
 * @param[in]  privkey      Clé privée brute (doit faire 64 octets)
 * @param[in]  privkey_len  Taille du buffer privkey (doit valoir 64)
 * @param[in]  pubkey       Clé publique brute (doit faire 32 octets)
 * @param[in]  pubkey_len   Taille du buffer pubkey (doit valoir 32)
 * @return ESP_OK si cohérent, ESP_ERR_INVALID_ARG si incohérent / taille
 *         incorrecte / NULL
 */
esp_err_t crypto_import_keypair(keypair_t *keypair,
                                const uint8_t *privkey, size_t privkey_len,
                                const uint8_t *pubkey,  size_t pubkey_len);

#endif /* CRYPTO_KEYS_H */
