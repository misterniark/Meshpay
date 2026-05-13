/**
 * @file dag_glue.c
 * @brief Implementation insertion DAG + checkpoint auto (voir dag_glue.h).
 */

#include "dag_glue.h"

#include <inttypes.h>
#include <string.h>

#include "esp_log.h"

#include "app_state.h"
#include "currency/currency_melt.h"
#include "dag/dag.h"
#include "dag/dag_prune.h"
#include "time_glue.h"
#include "wallet/wallet_checkpoint.h"

static const char *TAG = "dag_glue";

void auto_checkpoint_if_needed(void)
{
    if (!dag_needs_checkpoint(&s_dag)) {
        return;
    }

    checkpoint_t new_chk;
    esp_err_t ret = checkpoint_create(&s_dag, &s_checkpoint,
                                      &s_currency.mint_authorities[0], &new_chk);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Echec creation checkpoint automatique (%d)", ret);
        return;
    }

    /*
     * Fonte globale appliquee a tous les comptes du checkpoint quand
     * le temps maitre est dispo. Un seul timestamp global suffit
     * (meme periode pour tous). Tous les devices appliquent la meme
     * formule, donc convergent.
     */
    if (s_currency.melt_enabled &&
        s_time_manager.mode == TIME_MODE_MASTER &&
        time_manager_has_valid_master(&s_time_manager)) {
        uint64_t now = get_time_ms_wrapper();
        uint64_t last_ts = s_checkpoint.last_melt_timestamp;
        uint32_t ticks = currency_melt_ticks_due(&s_currency, last_ts, now);
        if (ticks > 0) {
            for (uint32_t i = 0; i < new_chk.account_count; i++) {
                new_chk.accounts[i].balance =
                    currency_melt_apply(&s_currency,
                                        new_chk.accounts[i].balance, ticks);
            }
            new_chk.last_melt_timestamp =
                currency_melt_next_timestamp(&s_currency, last_ts, ticks, now);
            ESP_LOGI(TAG, "Fonte checkpoint: %"PRIu32" ticks", ticks);
        } else {
            new_chk.last_melt_timestamp = last_ts;
        }
    }

    memcpy(&s_checkpoint, &new_chk, sizeof(checkpoint_t));
    if (s_checkpoint_save) {
        s_checkpoint_save(&s_checkpoint, NULL);
    }

    /*
     * Elaguer le DAG : sans cet elagage, le DAG se remplit jusqu'a
     * DAG_MAX_TRANSACTIONS et bloque toute nouvelle insertion.
     */
    dag_prune_before(&s_dag, new_chk.timestamp);

    ESP_LOGI(TAG, "Checkpoint automatique cree + DAG elague "
             "(reste %"PRIu32" TX)", s_dag.count);
}

esp_err_t dag_insert_and_track(const transaction_t *tx)
{
    esp_err_t ret = dag_insert(&s_dag, tx);
    if (ret != ESP_OK) {
        return ret;
    }
    auto_checkpoint_if_needed();
    return ESP_OK;
}
