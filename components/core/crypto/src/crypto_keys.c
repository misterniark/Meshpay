/**
 * @file crypto_keys.c
 * @brief Implémentation de la gestion des clés Ed25519 via Monocypher.
 *
 * Depuis le Lot E.2 (mai 2026) : remplace l'ancienne implémentation PSA
 * Crypto de mbedTLS, qui ne fournissait pas de driver Ed25519 dans IDF
 * v5.4.3 (`psa_generate_key` retournait PSA_ERROR_NOT_SUPPORTED = -134).
 *
 * Monocypher 4.0.2 est vendoré dans `vendor/monocypher/` et fournit
 * Ed25519 standard RFC 8032 (SHA-512). La randomness vient du TRNG
 * matériel ESP32 via `esp_fill_random()`.
 *
 * Format de la clé privée stockée : `seed[32] || public[32]` = 64 octets.
 * C'est le format `secret_key[64]` natif de Monocypher.
 */

#include "crypto/crypto_keys.h"
#include "crypto/crypto_init.h"
#include "crypto/crypto_sign.h"

/* Monocypher (vendor, headers privés au composant crypto) */
#include "monocypher-ed25519.h"

#include "esp_random.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "crypto_keys";

esp_err_t crypto_generate_keypair(keypair_t *keypair)
{
    if (keypair == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /*
     * Vérification que crypto_init() a été appelé au démarrage.
     * C'est une précondition obligatoire depuis la correction C6.
     * Monocypher ne demande pas d'init globale, mais on garde
     * cette garde pour maintenir l'invariant des callers existants.
     */
    if (!crypto_is_initialized()) {
        return ESP_ERR_INVALID_STATE;
    }

    /*
     * Génération du seed Ed25519 (32 octets de randomness).
     * `esp_fill_random()` utilise le TRNG matériel ESP32 (idem que
     * l'ancienne implémentation via PSA, qui appelait au final
     * la même source via mbedtls_psa_external_get_random).
     */
    uint8_t seed[32];
    esp_fill_random(seed, sizeof(seed));

    /*
     * `crypto_ed25519_key_pair` écrit :
     *   - secret_key[0..31]  = seed (copie)
     *   - secret_key[32..63] = public_key dérivée du seed
     *   - public_key[0..31]  = public_key dérivée
     * Et zéroise `seed` en sortie (defense in depth contre le leak).
     *
     * Le format `secret_key[64]` est exactement celui attendu par
     * `keypair_t.private_key` : aucune conversion nécessaire.
     */
    crypto_ed25519_key_pair(keypair->private_key,
                            keypair->public_key.bytes,
                            seed);

    /*
     * Defense in depth : `seed` est censé être déjà zéroïsé par
     * Monocypher mais on s'assure sur le pile locale au cas où le
     * compilateur l'optimiserait. Note: en pratique
     * `crypto_ed25519_key_pair` appelle `crypto_wipe()` qui passe
     * par une assembly memory barrier.
     */
    crypto_wipe(seed, sizeof(seed));

    ESP_LOGD(TAG, "Keypair Ed25519 generee (32B seed -> 64B secret + 32B public)");
    return ESP_OK;
}

esp_err_t crypto_export_public_key(const keypair_t *keypair, public_key_t *pubkey)
{
    if (keypair == NULL || pubkey == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memcpy(pubkey->bytes, keypair->public_key.bytes, CRYPTO_PUBLIC_KEY_SIZE);
    return ESP_OK;
}

esp_err_t crypto_import_keypair(keypair_t *keypair,
                                const uint8_t *privkey, size_t privkey_len,
                                const uint8_t *pubkey,  size_t pubkey_len)
{
    if (keypair == NULL || privkey == NULL || pubkey == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /*
     * [F-CR-003] Vérifier la taille des buffers contre les constantes
     * de format. Un NVS corrompu peut produire un blob plus court ;
     * sans ce check, le `memcpy` lirait au-delà du buffer et corromprait
     * la keypair silencieusement.
     */
    if (privkey_len != CRYPTO_PRIVATE_KEY_SIZE ||
        pubkey_len  != CRYPTO_PUBLIC_KEY_SIZE) {
        return ESP_ERR_INVALID_ARG;
    }

    /*
     * Vérification que crypto_init() a été appelé au démarrage.
     * Nécessaire pour la vérification cryptographique sign/verify (H14).
     */
    if (!crypto_is_initialized()) {
        return ESP_ERR_INVALID_STATE;
    }

    /*
     * Copie temporaire dans la structure keypair pour pouvoir
     * effectuer la vérification cryptographique.
     */
    memcpy(keypair->private_key, privkey, CRYPTO_PRIVATE_KEY_SIZE);
    memcpy(keypair->public_key.bytes, pubkey, CRYPTO_PUBLIC_KEY_SIZE);

    /*
     * Vérification de cohérence cryptographique (H14) :
     * Au lieu de simplement comparer privkey[32..63] avec pubkey
     * (ce qui est contournable en forgeant une clé privée avec la
     * bonne clé publique collée à la fin), on effectue un vrai
     * test sign/verify pour valider que la clé privée correspond
     * bien à la clé publique.
     */
    static const uint8_t test_msg[] = "keypair_verify";
    signature_t test_sig;

    /* Signature du message de test avec la keypair importée */
    esp_err_t err = crypto_sign(test_msg, sizeof(test_msg) - 1, keypair, &test_sig);
    if (err != ESP_OK) {
        /* La clé privée est invalide ou incompatible, on nettoie */
        /*
         * [F-CR-010] Effacement uniforme via `crypto_wipe` (avec memory
         * barrier Monocypher) au lieu de `memset` qui peut être éliminé
         * par le compilateur sur les chemins d'erreur.
         */
        crypto_wipe(keypair->private_key, sizeof(keypair->private_key));
        crypto_wipe(keypair->public_key.bytes, sizeof(keypair->public_key.bytes));
        return ESP_ERR_INVALID_ARG;
    }

    /* Vérification de la signature avec la clé publique fournie */
    err = crypto_verify(test_msg, sizeof(test_msg) - 1, &keypair->public_key, &test_sig);

    /* Zéroïsation de la signature de test (donnée sensible) */
    crypto_wipe(test_sig.bytes, sizeof(test_sig.bytes));

    if (err != ESP_OK) {
        /*
         * La signature ne correspond pas à la clé publique :
         * la paire de clés est incohérente.
         */
        /*
         * [F-CR-010] Effacement uniforme via `crypto_wipe` (avec memory
         * barrier Monocypher) au lieu de `memset` qui peut être éliminé
         * par le compilateur sur les chemins d'erreur.
         */
        crypto_wipe(keypair->private_key, sizeof(keypair->private_key));
        crypto_wipe(keypair->public_key.bytes, sizeof(keypair->public_key.bytes));
        return ESP_ERR_INVALID_ARG;
    }

    return ESP_OK;
}
