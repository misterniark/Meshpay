/**
 * @file crypto_sign.h
 * @brief Signature et vérification du profil crypto Mesh Pay.
 *
 * Chaque transaction est signée par l'émetteur avec sa clé privée Mesh Pay.
 * La signature couvre le contenu sérialisé de la transaction (tous les champs
 * sauf id et signature). Le profil actuel est Monocypher 4.0.2 ferme, expose
 * par crypto_profile.h ; ne pas annoncer d'interop RFC8032 sans migration.
 */

#ifndef CRYPTO_SIGN_H
#define CRYPTO_SIGN_H

#include "crypto/crypto_types.h"
#include "esp_err.h"
#include <stddef.h>

/**
 * @brief Signe un buffer avec une clé privée Mesh Pay.
 *
 * Produit une signature de 64 octets. Le buffer signé est typiquement
 * le contenu sérialisé de la transaction (sans id ni signature).
 *
 * @param[in]  data       Données à signer
 * @param[in]  data_len   Taille des données en octets
 * @param[in]  keypair    Paire de clés de l'émetteur (la clé privée est utilisée)
 * @param[out] signature  Signature résultante (64 octets)
 * @return ESP_OK en cas de succès, ESP_FAIL en cas d'erreur
 */
esp_err_t crypto_sign(const uint8_t *data, size_t data_len,
                      const keypair_t *keypair, signature_t *signature);

/**
 * @brief Vérifie une signature Mesh Pay avec une clé publique.
 *
 * Vérifie que la signature correspond bien aux données et à la clé
 * publique fournie. Utilisé pour valider l'authenticité d'une
 * transaction reçue d'un autre device.
 *
 * @param[in] data      Données signées
 * @param[in] data_len  Taille des données en octets
 * @param[in] pubkey    Clé publique de l'émetteur supposé
 * @param[in] signature Signature à vérifier
 * @return ESP_OK si la signature est valide, ESP_ERR_INVALID_STATE si invalide
 */
esp_err_t crypto_verify(const uint8_t *data, size_t data_len,
                        const public_key_t *pubkey, const signature_t *signature);

#endif /* CRYPTO_SIGN_H */
