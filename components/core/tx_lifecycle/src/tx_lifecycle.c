#include "tx_lifecycle/tx_lifecycle.h"

#include "crypto/crypto_sign.h"
#include "transaction/tx_types.h"

static esp_err_t status_terminal_guard(const transaction_t *tx,
                                       tx_status_t target)
{
    if (tx == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    if (tx->status == target) {
        return ESP_OK;
    }
    if (tx->status == TX_STATUS_CONFIRMED ||
        tx->status == TX_STATUS_CANCELLED) {
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
}

esp_err_t tx_lifecycle_confirm_by_ack(dag_t *dag,
                                      lock_table_t *locks,
                                      const hash_t *tx_id,
                                      const public_key_t *ack_sender)
{
    if (dag == NULL || tx_id == NULL || ack_sender == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    const transaction_t *tx = dag_get_by_id(dag, tx_id);
    esp_err_t ret = status_terminal_guard(tx, TX_STATUS_CONFIRMED);
    if (ret != ESP_OK) {
        return ret;
    }
    if (!public_key_equal(&tx->to, ack_sender)) {
        return ESP_ERR_INVALID_STATE;
    }
    if (tx->status == TX_STATUS_CONFIRMED) {
        return ESP_OK;
    }

    if (locks != NULL) {
        ret = lock_table_confirm(locks, tx_id);
        if (ret != ESP_OK) {
            return ret;
        }
    }
    return dag_set_status(dag, tx_id, TX_STATUS_CONFIRMED);
}

esp_err_t tx_lifecycle_confirm_received_transfer(dag_t *dag,
                                                 const hash_t *tx_id,
                                                 const public_key_t *own_key)
{
    if (dag == NULL || tx_id == NULL || own_key == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    const transaction_t *tx = dag_get_by_id(dag, tx_id);
    esp_err_t ret = status_terminal_guard(tx, TX_STATUS_CONFIRMED);
    if (ret != ESP_OK) {
        return ret;
    }
    if (tx->type != TX_TYPE_TRANSFER ||
        !public_key_equal(&tx->to, own_key)) {
        return ESP_ERR_INVALID_STATE;
    }
    if (tx->status == TX_STATUS_CONFIRMED) {
        return ESP_OK;
    }
    return dag_set_status(dag, tx_id, TX_STATUS_CONFIRMED);
}

esp_err_t tx_lifecycle_confirm_by_attestation(
    dag_t *dag,
    const comm_msg_attestation_t *att,
    tx_lifecycle_confirm_reason_t reason)
{
    (void)reason;
    if (dag == NULL || att == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (comm_msg_verify_attestation(att) != 0) {
        return ESP_ERR_INVALID_ARG;
    }

    const transaction_t *tx = dag_get_by_id(dag, &att->tx_id);
    esp_err_t ret = status_terminal_guard(tx, TX_STATUS_CONFIRMED);
    if (ret != ESP_OK) {
        return ret;
    }
    if (!public_key_equal(&att->attester_key, &tx->to)) {
        return ESP_ERR_INVALID_STATE;
    }
    if (tx->status == TX_STATUS_CONFIRMED) {
        return ESP_OK;
    }
    return dag_set_status(dag, &att->tx_id, TX_STATUS_CONFIRMED);
}

esp_err_t tx_lifecycle_replay_attestations(dag_t *dag,
                                           const comm_msg_attestation_t *atts,
                                           uint32_t count,
                                           uint32_t *out_applied)
{
    if (out_applied != NULL) {
        *out_applied = 0;
    }
    if (dag == NULL || (atts == NULL && count > 0)) {
        return ESP_ERR_INVALID_ARG;
    }

    uint32_t applied = 0;
    esp_err_t last_err = ESP_OK;
    for (uint32_t i = 0; i < count; i++) {
        const transaction_t *before = dag_get_by_id(dag, &atts[i].tx_id);
        bool was_confirmed = before != NULL &&
                             before->status == TX_STATUS_CONFIRMED;
        esp_err_t ret = tx_lifecycle_confirm_by_attestation(
            dag, &atts[i], TX_LIFECYCLE_CONFIRM_REPLAYED_ATTESTATION);
        const transaction_t *after = dag_get_by_id(dag, &atts[i].tx_id);
        bool is_confirmed = after != NULL &&
                            after->status == TX_STATUS_CONFIRMED;
        if (ret == ESP_OK && !was_confirmed && is_confirmed) {
            applied++;
        } else if (ret != ESP_ERR_NOT_FOUND) {
            last_err = ret;
        }
    }

    if (out_applied != NULL) {
        *out_applied = applied;
    }
    return last_err;
}

esp_err_t tx_lifecycle_cancel(dag_t *dag,
                              lock_table_t *locks,
                              const hash_t *tx_id,
                              tx_lifecycle_cancel_reason_t reason)
{
    (void)reason;
    if (dag == NULL || tx_id == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    const transaction_t *tx = dag_get_by_id(dag, tx_id);
    esp_err_t ret = status_terminal_guard(tx, TX_STATUS_CANCELLED);
    if (ret != ESP_OK) {
        return ret;
    }
    if (tx->status == TX_STATUS_CANCELLED) {
        return ESP_OK;
    }

    if (locks != NULL) {
        (void)lock_table_cancel(locks, tx_id);
    }
    return dag_set_status(dag, tx_id, TX_STATUS_CANCELLED);
}
