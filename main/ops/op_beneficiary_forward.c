/**
 * @file ops/op_beneficiary_forward.c
 * @brief Auto-forward periodique du solde vers le beneficiaire configure.
 *
 * Appele par core_task sous s_state_mutex quand `s_forward_interval_min > 0`.
 * Cree une TX TRANSFER auto-confirmee (status = CONFIRMED) — pas d'ACK
 * necessaire puisque c'est le device qui s'envoie a lui-meme la
 * confirmation. La TX se propage via lora_sync_task.
 */

#include "ops.h"

#include <inttypes.h>
#include <string.h>

#include "esp_log.h"

#include "app_state.h"
#include "balance.h"
#include "dag_glue.h"
#include "persistence/nvs_next_seq.h"
#include "time_glue.h"
#include "transaction/tx_create.h"
#include "wallet/wallet_lock.h"

static const char *TAG = "op_fwd";

void attempt_beneficiary_forward(void)
{
    uint32_t available = 0;
    compute_owner_balance(&available);
    apply_pending_melt(&available);

    uint32_t total_locked = 0;
    lock_table_total_locked(&s_lock_table, &total_locked);

    uint32_t spendable = (available > total_locked) ? available - total_locked : 0;

    if (spendable <= s_currency.transfer_fee) {
        ESP_LOGD(TAG, "Forward: solde insuffisant (%"PRIu32" <= fee %"PRIu32")",
                 spendable, s_currency.transfer_fee);
        return;
    }

    uint32_t forward_amount = spendable - s_currency.transfer_fee;
    if (s_currency.min_transfer_amount > 0 &&
        forward_amount < s_currency.min_transfer_amount) {
        ESP_LOGD(TAG, "Forward: montant trop faible (%"PRIu32" < min %"PRIu32")",
                 forward_amount, s_currency.min_transfer_amount);
        return;
    }
    if (s_currency.max_transfer_amount > 0 &&
        forward_amount > s_currency.max_transfer_amount) {
        forward_amount = s_currency.max_transfer_amount;
    }

    const transaction_t *tips[2];
    uint32_t tip_count = 0;
    dag_get_tips(&s_dag, tips, 2, &tip_count);
    if (tip_count == 0) {
        ESP_LOGW(TAG, "Forward: pas de tips dans le DAG");
        return;
    }
    hash_t parents[2];
    uint8_t parent_count = (tip_count > 2) ? 2 : (uint8_t)tip_count;
    for (uint8_t i = 0; i < parent_count; i++) {
        memcpy(&parents[i], &tips[i]->id, sizeof(hash_t));
    }

    uint64_t timestamp = get_tx_timestamp_wrapper();
    transaction_t tx;
    esp_err_t ret = tx_create_transfer(&tx, &s_keypair, &s_beneficiary_key,
                                       forward_amount, s_currency.currency_id,
                                       s_currency.transfer_fee,
                                       next_seq(),
                                       parents, parent_count, timestamp);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Forward: erreur creation TX: %d", ret);
        return;
    }

    /*
     * Auto-confirmation : status n'est pas couvert par la signature,
     * peut etre modifie. Meme pattern que les TX MINT.
     */
    tx.status = TX_STATUS_CONFIRMED;

    /*
     * [F-MN-005] Décision design 2026-05-16 : passer par lock_table
     * comme `op_payment` pour cohérence d'API et défense en profondeur.
     * Le lock est immédiatement confirmé (TX déjà CONFIRMED) — pas de
     * fenêtre d'attente d'ACK puisque le forward est auto-confirmé.
     * Cela protège contre une sur-dépense théorique si un autre paiement
     * en concurrence (même itération core_task) recalcule `total_locked`
     * sans voir cette TX.
     */
    ret = lock_table_lock(&s_lock_table, &tx.id, forward_amount);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Forward: erreur lock: %d", ret);
        return;
    }

    ret = dag_insert_and_track(&tx);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Forward: erreur insertion DAG: %d — rollback lock", ret);
        (void)lock_table_cancel(&s_lock_table, &tx.id);
        return;
    }

    /* [F-MN-005] La TX est CONFIRMED dès l'insertion, on libère le lock
     * immédiatement (cf. cycle confirm de lock_table). */
    (void)lock_table_confirm(&s_lock_table, &tx.id);

    ESP_LOGI(TAG, "Auto-forward: %"PRIu32" credits transferes au beneficiaire",
             forward_amount);
}
