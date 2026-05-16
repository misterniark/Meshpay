/**
 * @file handlers/handler_payment.c
 * @brief Handlers du flux de paiement : peer, tx, ack, timeout, attestation.
 *
 * Tous appeles depuis core_task sous s_state_mutex.
 */

#include "handlers.h"

#include <inttypes.h>
#include <string.h>

#include "esp_log.h"

#include "app_state.h"  /* doit etre inclus avant freertos/queue.h */
#include "freertos/queue.h"
#include "balance.h"
#include "crypto/crypto_sign.h"
#include "dag/dag_merge.h"
#include "dag_glue.h"
#include "peers.h"
#include "time_glue.h"
#include "transport/transport_lora.h"
#include "wallet/wallet.h"
#include "wallet/wallet_lock.h"
#include "currency/currency_rules.h"

static const char *TAG = "h_pay";

void handle_peer_discovered(const comm_event_t *evt)
{
    add_peer(&evt->data.peer);
}

void handle_tx_received(const comm_event_t *evt)
{
    const transaction_t *rx_tx = &evt->data.tx;

    /*
     * [C2-fix] Validation currency a partir du solde de l'emetteur :
     * defense en profondeur. Garanties anti-double-depense reelles :
     * lock_source + nonce monotone (I3).
     */
    uint32_t sender_balance = 0;
    const public_key_t *fee_recipient =
        (s_currency.mint_authority_count > 0)
            ? &s_currency.mint_authorities[0] : NULL;
    wallet_get_balance_for(&s_dag, &s_checkpoint, &rx_tx->from,
                           fee_recipient, &sender_balance);

    uint64_t total_minted = 0;
    wallet_get_total_minted(&s_dag, &total_minted);

    currency_err_t cerr = currency_validate(&s_currency, rx_tx,
                                            get_time_ms_wrapper(),
                                            sender_balance, total_minted, 0);
    if (cerr != CURRENCY_OK) {
        ESP_LOGW(TAG, "TX recue : regle currency violee (%d)", cerr);
        return;
    }

    /*
     * [F-CU-001] Vérification explicite de l'autorité MINT.
     *
     * `currency_validate` est partielle (cf. [H6] header) et ne vérifie
     * pas que le signataire d'un MINT est une autorité reconnue.
     * `dag_merge_transaction` rattrape cette garde via `tx_validate_master`,
     * mais le couplage est implicite. On vérifie ici directement
     * l'autorité avant le merge pour rendre la chaîne de validation
     * explicite et à l'épreuve d'un refactoring qui changerait le chemin
     * en aval.
     */
    if (rx_tx->type == TX_TYPE_MINT) {
        cerr = currency_check_mint_authority(&s_currency, &rx_tx->from);
        if (cerr != CURRENCY_OK) {
            ESP_LOGW(TAG, "TX MINT recue : signataire non autorite (%d)", cerr);
            return;
        }
    }

    master_keys_t mk = {
        .keys  = s_currency.mint_authorities,
        .count = s_currency.mint_authority_count,
    };

    dag_merge_result_t merge_result;
    esp_err_t ret = dag_merge_transaction(&s_dag, rx_tx, &mk, &merge_result);

    if (merge_result == DAG_MERGE_DUPLICATE) {
        return;
    }
    if (merge_result != DAG_MERGE_INSERTED) {
        ESP_LOGW(TAG, "TX recue : merge rejete (result=%d, err=%d)",
                 merge_result, ret);
        return;
    }

    auto_checkpoint_if_needed();
    ESP_LOGI(TAG, "TX recue et inseree (amount=%"PRIu32")", rx_tx->amount);

    /* TRANSFER vers nous → confirmer + ACK ESP-NOW + attestation LoRa. */
    if (rx_tx->type == TX_TYPE_TRANSFER &&
        public_key_equal(&rx_tx->to, &s_keypair.public_key)) {

        dag_set_status(&s_dag, &rx_tx->id, TX_STATUS_CONFIRMED);

        const uint8_t *dest_mac = find_peer_mac(&rx_tx->from);
        if (dest_mac != NULL) {
            comm_cmd_t cmd;
            memset(&cmd, 0, sizeof(cmd));
            cmd.type = COMM_CMD_SEND_ACK;
            memcpy(&cmd.data.send_ack.tx_id, &rx_tx->id, sizeof(hash_t));
            memcpy(cmd.data.send_ack.dest_mac, dest_mac, 6);
            xQueueSend(s_cmd_queue, &cmd, 0);
        }

        /*
         * [I2-fix] ATTESTATION signee LoRa. Permet aux pairs hors portee
         * ESP-NOW (~200 m) de savoir que la TX est confirmee. Sans cela
         * les TX LoRa restent LOCKED ad vitam (status force a LOCKED en
         * reception, anti-usurpation).
         */
        signature_t att_sig;
        if (crypto_sign(rx_tx->id.bytes, CRYPTO_HASH_SIZE,
                        &s_keypair, &att_sig) == ESP_OK) {
            uint8_t att_buf[COMM_MSG_ATTESTATION_SIZE];
            size_t att_len = 0;
            if (comm_msg_pack_attestation(att_buf, sizeof(att_buf),
                                          &s_keypair.public_key,
                                          &att_sig, &rx_tx->id,
                                          &att_len) == 0) {
                transport_lora_send(att_buf, att_len, "attestation");
            }
        } else {
            ESP_LOGE(TAG, "Echec signature attestation");
        }

        ESP_LOGI(TAG, "TX confirmee + ACK/attestation diffuses (amount=%"PRIu32")",
                 rx_tx->amount);
    }

    time_manager_on_tx_received(&s_time_manager, rx_tx->timestamp);
}

void handle_ack_received(const comm_event_t *evt)
{
    const hash_t       *tx_id      = &evt->data.ack.tx_id;
    const public_key_t *sender_key = &evt->data.ack.sender_key;

    /*
     * [C4-fix] L'ACK doit etre signe par tx.to (le destinataire) — sinon
     * c'est un faux accuse de reception. La signature crypto a deja ete
     * verifiee par espnow.c ; on verifie ici l'IDENTITE du signataire.
     */
    const transaction_t *tx = dag_get_by_id(&s_dag, tx_id);
    if (tx == NULL) {
        ESP_LOGW(TAG, "ACK recu pour TX inconnue dans le DAG");
        return;
    }

    if (!public_key_equal(&tx->to, sender_key)) {
        ESP_LOGW(TAG, "ACK rejete : sender_key != tx.to (forge d'ACK ?)");
        return;
    }

    esp_err_t ret = lock_table_confirm(&s_lock_table, tx_id);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "ACK recu pour verrou inconnu");
        return;
    }

    dag_set_status(&s_dag, tx_id, TX_STATUS_CONFIRMED);
    ESP_LOGI(TAG, "TX confirmee (ACK recu et verifie)");
}

void handle_tx_timeout(const comm_event_t *evt)
{
    const hash_t *tx_id = &evt->data.tx_id;
    lock_table_cancel(&s_lock_table, tx_id);
    dag_set_status(&s_dag, tx_id, TX_STATUS_CANCELLED);
    ESP_LOGW(TAG, "TX annulee (timeout)");
}

void handle_attestation_received(const comm_event_t *evt)
{
    const comm_msg_attestation_t *att = &evt->data.attestation;

    const transaction_t *tx = dag_get_by_id(&s_dag, &att->tx_id);
    if (tx == NULL) {
        ESP_LOGD(TAG, "Attestation reçue pour TX inconnue — ignorée");
        return;
    }
    if (tx->status == TX_STATUS_CONFIRMED) {
        return;
    }
    if (tx->status == TX_STATUS_CANCELLED) {
        ESP_LOGW(TAG, "Attestation reçue pour TX annulée — ignorée");
        return;
    }
    if (!public_key_equal(&att->attester_key, &tx->to)) {
        ESP_LOGW(TAG, "Attestation rejetée : attester_key != tx.to "
                 "(tentative d'attestation par un tiers)");
        return;
    }

    /*
     * [F-MN-015] Vérification locale de la signature de l'attestation
     * en défense en profondeur. La signature est normalement déjà
     * vérifiée par `lora_sync_task` (via `comm_msg_verify_attestation`)
     * avant que l'événement soit posté dans la queue, mais on
     * re-vérifie ici pour fermer tout chemin alternatif (tests,
     * injection directe). Le payload signé est exactement `tx_id`
     * (32 octets) — voir `comm_msg.h` pour le format.
     */
    esp_err_t verr = crypto_verify(att->tx_id.bytes, CRYPTO_HASH_SIZE,
                                   &att->attester_key, &att->signature);
    if (verr != ESP_OK) {
        ESP_LOGW(TAG, "Attestation rejetee : signature invalide");
        return;
    }

    dag_set_status(&s_dag, &att->tx_id, TX_STATUS_CONFIRMED);
    ESP_LOGI(TAG, "TX confirmée par attestation LoRa (amount=%"PRIu32")",
             tx->amount);
}
