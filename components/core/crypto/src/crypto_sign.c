/**
 * @file crypto_sign.c
 * @brief Implémentation de la signature Mesh Pay via Monocypher.
 *
 * Depuis le Lot E.2 (mai 2026) : remplace l'ancienne implémentation
 * basée sur PSA Crypto de mbedTLS (`PSA_ALG_PURE_EDDSA`), qui retournait
 * `PSA_ERROR_NOT_SUPPORTED` en runtime car la lib mbedTLS d'IDF v5.4.3
 * ne fournit aucun driver Ed25519.
 *
 * Monocypher (vendoré dans `vendor/monocypher/`) fournit l'API
 * `crypto_ed25519_*` sans dépendance externe. Suite à la vérification du
 * 2026-05-17, ce profil est explicitement traité comme un format fermé
 * Mesh Pay / Monocypher 4.0.2, pas comme une promesse d'interop RFC8032.
 * La signature reste sur le fil exactement la même (64 octets).
 *
 * L'initialisation `crypto_init()` reste appelée au démarrage et le flag
 * `crypto_is_initialized()` reste consulté ici, pour conserver l'invariant
 * introduit par la correction C6 (audit Sonnet).
 */

#include "crypto/crypto_sign.h"
#include "crypto/crypto_init.h"

/* Monocypher (vendor, headers privés au composant crypto) */
#include "monocypher-ed25519.h"

#include <string.h>

esp_err_t crypto_sign(const uint8_t *data, size_t data_len,
                      const keypair_t *keypair, signature_t *signature)
{
    if (data == NULL || keypair == NULL || signature == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /*
     * [F-CR-006] Refuser un message vide. Dans Mesh Pay, une TX ne peut
     * pas avoir un payload signable de taille 0 — `data_len == 0` indique
     * un bug amont (`tx_serialize_signable` retournant 0 sans propager
     * d'erreur).
     */
    if (data_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    /*
     * Vérification que crypto_init() a été appelé au démarrage.
     * Garde maintenue pour respecter l'invariant C6 même si Monocypher
     * ne necessite plus d'init globale (pas d'etat partage interne).
     */
    if (!crypto_is_initialized()) {
        return ESP_ERR_INVALID_STATE;
    }

    /*
     * Signature via l'API `crypto_ed25519_*` Monocypher.
     * `keypair->private_key` est au format `seed[32] || public[32]`,
     * exactement le format `secret_key[64]` attendu par Monocypher.
     * La fonction est `void` : aucun chemin d'erreur cryptographique
     * tant que les pointeurs sont valides et les tailles correctes.
     */
    crypto_ed25519_sign(signature->bytes,
                        keypair->private_key,
                        data,
                        data_len);

    return ESP_OK;
}

esp_err_t crypto_verify(const uint8_t *data, size_t data_len,
                        const public_key_t *pubkey, const signature_t *signature)
{
    if (data == NULL || pubkey == NULL || signature == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /*
     * [F-CR-006] Refuser un message vide. Cohérent avec `crypto_sign` :
     * une signature valide pour un message vide est suspecte dans Mesh
     * Pay (bug amont qui aurait produit cette signature).
     */
    if (data_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    /*
     * Vérification que crypto_init() a été appelé au démarrage.
     * Maintenu pour la cohérence avec crypto_sign (invariant C6).
     */
    if (!crypto_is_initialized()) {
        return ESP_ERR_INVALID_STATE;
    }

    /*
     * Vérification via l'API `crypto_ed25519_*` Monocypher.
     * `crypto_ed25519_check` retourne :
     *   -  0 si la signature est valide
     *   - -1 si la signature est invalide
     * On mappe sur l'API du composant : ESP_OK = valide,
     * ESP_ERR_INVALID_STATE = signature invalide (conforme au
     * contrat existant defini dans crypto_sign.h).
     */
    int rc = crypto_ed25519_check(signature->bytes,
                                  pubkey->bytes,
                                  data,
                                  data_len);

    return (rc == 0) ? ESP_OK : ESP_ERR_INVALID_STATE;
}
