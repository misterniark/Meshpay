/**
 * @file ops/op_payment.c
 * @brief Initie un paiement TRANSFER (depuis l'UI).
 *
 * [C1-fix] Appele avec s_state_mutex DEJA pris par l'appelant (core_task).
 * Le mutex n'est pas recursif, le reprendre ici provoquerait un deadlock.
 */

#include "ops.h"

#include <inttypes.h>
#include <string.h>

#include "esp_log.h"

#include "app_state.h"  /* avant freertos/queue.h */
#include "freertos/queue.h"

#include "balance.h"
#include "dag_glue.h"
#include "peers.h"
#include "persistence/nvs_next_seq.h"
#include "time_glue.h"
#include "transaction/tx_create.h"
#include "wallet/wallet_lock.h"

static const char *TAG = "op_pay";

esp_err_t initiate_payment(const public_key_t *to, uint32_t amount)
{
    if (to == NULL || amount == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = ESP_FAIL;

    /* 1. Verifier solde disponible (checkpoint + DAG + fonte). */
    uint32_t available = 0;
    compute_owner_balance(&available);
    apply_pending_melt(&available);

    uint32_t total_locked = 0;
    lock_table_total_locked(&s_lock_table, &total_locked);

    uint32_t spendable = (available > total_locked) ? available - total_locked : 0;
    uint32_t total_cost = amount + s_currency.transfer_fee;

    if (spendable < total_cost) {
        ESP_LOGW(TAG, "Solde insuffisant: dispo=%"PRIu32" requis=%"PRIu32,
                 spendable, total_cost);
        return ESP_ERR_INVALID_STATE;
    }

    /* 2. Tips du DAG comme parents. */
    const transaction_t *tips[2];
    uint32_t tip_count = 0;
    dag_get_tips(&s_dag, tips, 2, &tip_count);
    if (tip_count == 0) {
        ESP_LOGE(TAG, "Pas de tips dans le DAG");
        return ESP_ERR_INVALID_STATE;
    }
    hash_t parents[2];
    uint8_t parent_count = (tip_count > 2) ? 2 : (uint8_t)tip_count;
    for (uint8_t i = 0; i < parent_count; i++) {
        memcpy(&parents[i], &tips[i]->id, sizeof(hash_t));
    }

    /* 3. Creer la TX signee. */
    uint64_t timestamp = get_tx_timestamp_wrapper();
    transaction_t tx;
    ret = tx_create_transfer(&tx, &s_keypair, to, amount,
                             s_currency.currency_id,
                             s_currency.transfer_fee,
                             next_seq(),
                             parents, parent_count, timestamp);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Erreur creation TX: %d", ret);
        return ret;
    }

    /* 4. Inserer dans le DAG (checkpoint auto). */
    ret = dag_insert_and_track(&tx);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Erreur insertion DAG: %d", ret);
        return ret;
    }

    /* 5. Verrouiller le montant cote emetteur (sera libere a l'ACK). */
    ret = lock_table_lock(&s_lock_table, &tx.id, amount);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Erreur verrouillage: %d", ret);
        return ret;
    }

    /* 6. Envoi ESP-NOW direct si peer connu, sinon attendre LoRa sync. */
    const uint8_t *dest_mac = find_peer_mac(to);
    if (dest_mac != NULL) {
        comm_cmd_t cmd;
        memset(&cmd, 0, sizeof(cmd));
        cmd.type = COMM_CMD_SEND_TX;
        memcpy(&cmd.data.send_tx.tx, &tx, sizeof(transaction_t));
        memcpy(cmd.data.send_tx.dest_mac, dest_mac, 6);
        xQueueSend(s_cmd_queue, &cmd, 0);
    } else {
        ESP_LOGW(TAG, "Destinataire non trouve dans la table des peers");
        /*
         * La TX est dans le DAG + locked, elle sera sync via LoRa sur les
         * cartes equipees (CYD). Sur les autres (Waveshare S3), elle reste
         * locale jusqu'a une future decouverte.
         */
    }

    ESP_LOGI(TAG, "Paiement initie: amount=%"PRIu32, amount);
    return ESP_OK;
}
