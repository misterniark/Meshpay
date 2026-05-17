/**
 * @file ops/op_mint.c
 * @brief Cree une TX MINT (maitre uniquement).
 *
 * Verifie que le device est present dans `s_currency.mint_authorities`.
 * Insere la TX MINT dans le DAG + checkpoint automatique.
 *
 * Appele sous s_state_mutex (cf. [C1-fix]).
 */

#include "ops.h"

#include <inttypes.h>
#include <string.h>

#include "esp_log.h"

#include "app_state.h"
#include "dag_glue.h"
#include "persistence/nvs_next_seq.h"
#include "time_glue.h"
#include "transaction/tx_create.h"

static const char *TAG = "op_mint";

esp_err_t initiate_mint(const public_key_t *to, uint32_t amount)
{
    if (to == NULL || amount == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    /*
     * [F-CU-009] Utiliser public_key_equal (temps constant) au lieu de
     * memcmp pour la comparaison de clés publiques. Comportement
     * fonctionnel identique mais aligné avec la convention de sécurité
     * établie dans le reste du projet (cf. crypto_types.h).
     */
    bool is_master = false;
    for (uint8_t i = 0; i < s_currency.mint_authority_count; i++) {
        if (public_key_equal(&s_keypair.public_key,
                             &s_currency.mint_authorities[i])) {
            is_master = true;
            break;
        }
    }
    if (!is_master) {
        ESP_LOGW(TAG, "MINT refuse : device non maitre");
        return ESP_ERR_NOT_ALLOWED;
    }

    /* Tips du DAG comme parents (genese si DAG vide). */
    const transaction_t *tips[2];
    uint32_t tip_count = 0;
    dag_get_tips(&s_dag, tips, 2, &tip_count);

    hash_t parents[2];
    uint8_t parent_count;
    if (tip_count == 0) {
        if (!hash_is_zero(&s_checkpoint.last_tx_id) &&
            s_checkpoint.timestamp > 0) {
            memcpy(&parents[0], &s_checkpoint.last_tx_id, sizeof(hash_t));
            parent_count = 1;
            ESP_LOGI(TAG, "MINT: parent issu du checkpoint (DAG vide)");
        } else {
            memset(&parents[0], 0, sizeof(hash_t));
            parent_count = 1;
        }
    } else {
        parent_count = (tip_count > 2) ? 2 : (uint8_t)tip_count;
        for (uint8_t i = 0; i < parent_count; i++) {
            memcpy(&parents[i], &tips[i]->id, sizeof(hash_t));
        }
    }

    transaction_t mint_tx;
    esp_err_t ret = tx_create_mint(&mint_tx, &s_keypair, to, amount,
                                   s_currency.currency_id,
                                   next_seq(),
                                   parents, parent_count,
                                   get_tx_timestamp_wrapper());
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Erreur creation MINT: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = dag_insert_and_track(&mint_tx);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Erreur insertion MINT dans DAG: %s", esp_err_to_name(ret));
    } else {
        persist_runtime_checkpoint("mint");
        ESP_LOGI(TAG, "MINT cree: %"PRIu32" credits", amount);
    }
    return ret;
}
