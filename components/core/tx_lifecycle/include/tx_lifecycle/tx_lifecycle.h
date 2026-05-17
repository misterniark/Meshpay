/**
 * @file tx_lifecycle.h
 * @brief Regles centrales du cycle de vie d'une transaction.
 *
 * Cette couche ne persiste rien et n'envoie aucun message radio. Elle
 * formalise seulement les transitions applicatives autorisees autour du
 * DAG: ACK, attestation, timeout et rollback local.
 */

#ifndef MESHPAY_TX_LIFECYCLE_H
#define MESHPAY_TX_LIFECYCLE_H

#include "comm/comm_msg.h"
#include "dag/dag.h"
#include "esp_err.h"
#include "wallet/wallet_lock.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    TX_LIFECYCLE_CONFIRM_ACK,
    TX_LIFECYCLE_CONFIRM_RECIPIENT,
    TX_LIFECYCLE_CONFIRM_ATTESTATION,
    TX_LIFECYCLE_CONFIRM_REPLAYED_ATTESTATION,
} tx_lifecycle_confirm_reason_t;

typedef enum {
    TX_LIFECYCLE_CANCEL_TIMEOUT,
    TX_LIFECYCLE_CANCEL_LOCAL_ROLLBACK,
    TX_LIFECYCLE_CANCEL_CHECKPOINT_GUARD,
} tx_lifecycle_cancel_reason_t;

/**
 * Confirme une TX via ACK ESP-NOW.
 *
 * Regle: l'ACK n'est valide que si son signataire est exactement `tx.to`.
 * Si la TX est deja CONFIRMED, l'appel est idempotent. Une TX CANCELLED
 * ne peut pas etre ressuscitee.
 */
esp_err_t tx_lifecycle_confirm_by_ack(dag_t *dag,
                                      lock_table_t *locks,
                                      const hash_t *tx_id,
                                      const public_key_t *ack_sender);

/**
 * Confirme une TX recue par son destinataire local.
 *
 * Regle: seul le destinataire (`tx.to`) peut promouvoir une TRANSFER
 * recue en CONFIRMED.
 */
esp_err_t tx_lifecycle_confirm_received_transfer(dag_t *dag,
                                                 const hash_t *tx_id,
                                                 const public_key_t *own_key);

/**
 * Applique une attestation signee.
 *
 * Regle: l'attestation doit signer tx_id et etre emise par `tx.to`.
 * Si la TX n'est pas encore connue, retourne ESP_ERR_NOT_FOUND pour que
 * l'appelant conserve l'attestation et la rejoue plus tard.
 */
esp_err_t tx_lifecycle_confirm_by_attestation(
    dag_t *dag,
    const comm_msg_attestation_t *att,
    tx_lifecycle_confirm_reason_t reason);

/**
 * Rejoue un lot d'attestations conservees.
 */
esp_err_t tx_lifecycle_replay_attestations(dag_t *dag,
                                           const comm_msg_attestation_t *atts,
                                           uint32_t count,
                                           uint32_t *out_applied);

/**
 * Annule une TX LOCKED.
 *
 * Regle: CONFIRMED et CANCELLED sont terminaux. L'annulation est
 * idempotente si la TX est deja CANCELLED.
 */
esp_err_t tx_lifecycle_cancel(dag_t *dag,
                              lock_table_t *locks,
                              const hash_t *tx_id,
                              tx_lifecycle_cancel_reason_t reason);

#ifdef __cplusplus
}
#endif

#endif /* MESHPAY_TX_LIFECYCLE_H */
