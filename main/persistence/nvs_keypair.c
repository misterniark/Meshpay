/**
 * @file persistence/nvs_keypair.c
 * @brief Implementation du load/generate keypair (voir header).
 */

#include "nvs_keypair.h"

#include <stdlib.h>

#include "esp_err.h"
#include "esp_log.h"

#include "app_state.h"
#include "crypto/crypto_keys.h"
#include "crypto/crypto_types.h"

static const char *TAG = "nvs_kp";

esp_err_t nvs_keypair_load_or_generate(void)
{
    size_t len = sizeof(s_keypair.private_key);
    bool key_exists = false;

    hal_err_t err = s_storage.exists(NVS_NAMESPACE, NVS_KEY_PRIVKEY,
                                     &key_exists, s_storage.ctx);
    if (err != HAL_OK) {
        ESP_LOGE(TAG, "Erreur verification cle NVS: %d", err);
        return ESP_FAIL;
    }

    if (key_exists) {
        /*
         * [F-MN-008, F-CR-004] On lit les deux blobs dans des buffers
         * temporaires (pas directement dans s_keypair) puis on appelle
         * `crypto_import_keypair` qui vérifie la cohérence par un test
         * sign/verify. Une corruption NVS partielle ne produit donc plus
         * un keypair silencieusement inutilisable.
         */
        uint8_t buf_priv[CRYPTO_PRIVATE_KEY_SIZE];
        uint8_t buf_pub[CRYPTO_PUBLIC_KEY_SIZE];

        size_t priv_len = sizeof(buf_priv);
        err = s_storage.blob_read(NVS_NAMESPACE, NVS_KEY_PRIVKEY,
                                  buf_priv, &priv_len, s_storage.ctx);
        if (err != HAL_OK) {
            ESP_LOGE(TAG, "Erreur lecture cle privee NVS: %d", err);
            return ESP_FAIL;
        }
        size_t pub_len = sizeof(buf_pub);
        err = s_storage.blob_read(NVS_NAMESPACE, NVS_KEY_PUBKEY,
                                  buf_pub, &pub_len, s_storage.ctx);
        if (err != HAL_OK) {
            ESP_LOGE(TAG, "Erreur lecture cle publique NVS: %d", err);
            return ESP_FAIL;
        }

        /*
         * [F-CR-004] Décision design 2026-05-16 : si le keypair NVS est
         * incohérent (corruption partielle, flash encryption échouée
         * sur un seul bloc), on appelle abort() au lieu de continuer
         * avec une identité corrompue. Le device refuse de booter,
         * forçant un reflash — préférable à un comportement
         * silencieusement cassé (toutes les TX rejetées par les peers
         * sans diagnostic local).
         */
        esp_err_t cret = crypto_import_keypair(&s_keypair,
                                               buf_priv, priv_len,
                                               buf_pub,  pub_len);
        if (cret != ESP_OK) {
            ESP_LOGE(TAG, "Keypair NVS incoherent (%s) — corruption "
                          "detectee. ABORT pour eviter une identite "
                          "fantome sur le reseau.",
                     esp_err_to_name(cret));
            abort();
        }
        ESP_LOGI(TAG, "Keypair charge depuis NVS et verifie (sign/verify OK)");
        (void)len;  /* gardé pour compat ; non utilisé désormais */
        return ESP_OK;
    }

    /* Premier boot : generer + sauvegarder. */
    esp_err_t ret = crypto_generate_keypair(&s_keypair);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Erreur generation keypair: %d", ret);
        return ret;
    }

    err = s_storage.blob_write(NVS_NAMESPACE, NVS_KEY_PRIVKEY,
                               s_keypair.private_key,
                               sizeof(s_keypair.private_key), s_storage.ctx);
    if (err != HAL_OK) {
        ESP_LOGE(TAG, "Erreur ecriture cle privee NVS: %d", err);
        return ESP_FAIL;
    }
    err = s_storage.blob_write(NVS_NAMESPACE, NVS_KEY_PUBKEY,
                               s_keypair.public_key.bytes,
                               sizeof(s_keypair.public_key), s_storage.ctx);
    if (err != HAL_OK) {
        ESP_LOGE(TAG, "Erreur ecriture cle publique NVS: %d", err);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Nouveau keypair genere et sauvegarde");
    return ESP_OK;
}
