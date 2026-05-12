/**
 * @file crypto_hash.c
 * @brief Implémentation du hachage SHA-256 via mbedtls.
 *
 * Utilise l'API mbedtls_sha256 pour calculer les hashes.
 * Sur ESP32, mbedtls bénéficie de l'accélération matérielle SHA.
 */

#include "crypto/crypto_hash.h"
#include "mbedtls/sha256.h"
#include <string.h>

esp_err_t crypto_hash_sha256(const uint8_t *data, size_t data_len, hash_t *hash)
{
    if (data == NULL || hash == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Autoriser le hash d'un buffer vide (résultat déterministe) */
    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);

    /* Le deuxième paramètre 0 = SHA-256 (pas SHA-224) */
    int ret = mbedtls_sha256_starts(&ctx, 0);
    if (ret != 0) {
        mbedtls_sha256_free(&ctx);
        return ESP_FAIL;
    }

    ret = mbedtls_sha256_update(&ctx, data, data_len);
    if (ret != 0) {
        mbedtls_sha256_free(&ctx);
        return ESP_FAIL;
    }

    ret = mbedtls_sha256_finish(&ctx, hash->bytes);
    mbedtls_sha256_free(&ctx);

    return (ret == 0) ? ESP_OK : ESP_FAIL;
}
