/**
 * @file persistence/ledger_store.h
 * @brief Persistance durable du ledger sur la partition data "storage".
 */

#ifndef MESHPAY_PERSISTENCE_LEDGER_STORE_H
#define MESHPAY_PERSISTENCE_LEDGER_STORE_H

#include "esp_err.h"
#include "comm/comm_msg.h"
#include "wallet/wallet_checkpoint.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t ledger_checkpoint_load(checkpoint_t *checkpoint, void *ctx);
esp_err_t ledger_checkpoint_save(const checkpoint_t *checkpoint, void *ctx);

esp_err_t ledger_tx_window_load_into_dag(void);
esp_err_t ledger_tx_window_save_from_dag(const char *reason);
esp_err_t ledger_tx_window_read_recent(transaction_t *out_txs,
                                       uint32_t max_count,
                                       uint32_t *out_count);

esp_err_t ledger_attestation_window_add(const comm_msg_attestation_t *att);
esp_err_t ledger_attestation_window_read_recent(comm_msg_attestation_t *out,
                                                uint32_t max_count,
                                                uint32_t *out_count);
esp_err_t ledger_attestation_apply_to_dag(uint32_t *out_applied);

#ifdef __cplusplus
}
#endif

#endif /* MESHPAY_PERSISTENCE_LEDGER_STORE_H */
