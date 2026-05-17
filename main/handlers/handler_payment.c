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
#include "sdkconfig.h"

#include "app_state.h"  /* doit etre inclus avant freertos/queue.h */
#include "freertos/queue.h"
#include "balance.h"
#include "crypto/crypto_sign.h"
#include "dag/dag_merge.h"
#include "dag_glue.h"
#include "peers.h"
#include "persistence/ledger_store.h"
#include "time_glue.h"
#include "transport/transport_lora.h"
#include "tx_lifecycle/tx_lifecycle.h"
#include "wallet/wallet.h"
#include "wallet/wallet_lock.h"
#include "currency/currency_rules.h"

static const char *TAG = "h_pay";

void handle_peer_discovered(const comm_event_t *evt)
{
    add_peer(&evt->data.peer);

    /*
     * [F-MN-016] Bootstrap propagation : envoyer au nouveau peer toutes
     * mes TX MINT signées par moi-même (mon solde initial déclaré au
     * boot via main.c étape 10). Sans cette propagation, le peer
     * ignore mon solde et rejette tout paiement entrant avec
     * `CURRENCY_ERR_INSUFFICIENT` car `wallet_get_balance_for(mon_pk)`
     * retourne 0 dans son DAG.
     *
     * On envoie via COMM_CMD_SEND_TX (le même chemin que les paiements
     * normaux). Le récepteur valide via `handle_tx_received` :
     * - signature OK (signée par moi)
     * - `currency_check_mint_authority` accepte le self-MINT
     *   (cf. F-TR-005 dans tx_validate.c)
     * - `dag_merge_transaction` dédupliqué via tx_id si re-discovery
     *
     * Limite : on n'envoie QUE les TX MINT (pas les TRANSFER que j'ai
     * faits). Pour un bootstrap minimal de paiement entre deux
     * devices, ça suffit. Pour une sync DAG complète, voir la sync
     * LoRa périodique (lora_sync_task).
     */
    const public_key_t *to = &evt->data.peer.public_key;
    const uint8_t      *dest_mac = evt->data.peer.mac_addr;

    uint32_t sent = 0;
#if CONFIG_MESHPAY_TEST_DEVICE_SEED
    transaction_t seed_tx;
    if (test_device_seed_get_tx(&seed_tx)) {
        comm_cmd_t cmd;
        memset(&cmd, 0, sizeof(cmd));
        cmd.type = COMM_CMD_SEND_TX;
        memcpy(&cmd.data.send_tx.tx, &seed_tx, sizeof(transaction_t));
        memcpy(cmd.data.send_tx.dest_mac, dest_mac, 6);
        if (xQueueSend(s_cmd_queue, &cmd, 0) == pdTRUE) {
            sent++;
        }
    }
#endif
    for (uint32_t i = 0; i < s_dag.count; i++) {
        const transaction_t *tx = &s_dag.transactions[i];
        /*
         * On propage uniquement nos propres MINT confirmés. Les MINTs
         * d'autres devices propagés à nous ne sont pas re-propagés ici
         * pour éviter les boucles de gossip ; la sync LoRa s'en charge.
         */
        if (tx->type != TX_TYPE_MINT)                       continue;
        if (!public_key_equal(&tx->from, &s_keypair.public_key)) continue;
        if (tx->status != TX_STATUS_CONFIRMED)              continue;
#if CONFIG_MESHPAY_TEST_DEVICE_SEED
        if (memcmp(&tx->id, &seed_tx.id, sizeof(hash_t)) == 0) continue;
#endif

        comm_cmd_t cmd;
        memset(&cmd, 0, sizeof(cmd));
        cmd.type = COMM_CMD_SEND_TX;
        memcpy(&cmd.data.send_tx.tx, tx, sizeof(transaction_t));
        memcpy(cmd.data.send_tx.dest_mac, dest_mac, 6);
        if (xQueueSend(s_cmd_queue, &cmd, 0) == pdTRUE) {
            sent++;
        }
    }
    if (sent > 0) {
        ESP_LOGI(TAG, "Bootstrap : %"PRIu32" MINT(s) envoye(s) au nouveau peer", sent);
    }
    /*
     * Suppression du warning unused-variable si la chaine de format
     * change : `to` est conservé pour usage futur (e.g. log de la
     * pubkey du peer).
     */
    (void)to;
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
        /*
         * [F-MN-016] Exception self-MINT : un MINT où from == to est
         * une auto-déclaration de solde par un peer (cf. F-TR-005 dans
         * tx_validate.c). On l'accepte sans vérifier l'autorité locale,
         * sinon la propagation au peer discovery ne marcherait jamais
         * (le peer n'est pas dans `s_currency.mint_authorities`).
         * `tx_validate_master` (appelé par `dag_merge_transaction`
         * en aval) applique la même exception, donc les deux gardes
         * restent alignées.
         */
        if (!public_key_equal(&rx_tx->from, &rx_tx->to)) {
            cerr = currency_check_mint_authority(&s_currency, &rx_tx->from);
            if (cerr != CURRENCY_OK) {
                ESP_LOGW(TAG, "TX MINT recue : signataire non autorite (%d)", cerr);
                return;
            }
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
    if (rx_tx->status == TX_STATUS_CONFIRMED) {
        persist_runtime_checkpoint("rx_confirmed");
    }
    ESP_LOGI(TAG, "TX recue et inseree (amount=%"PRIu32")", rx_tx->amount);

    uint32_t replayed_attest = 0;
    if (ledger_attestation_apply_to_dag(&replayed_attest) == ESP_OK &&
        replayed_attest > 0) {
        persist_runtime_checkpoint("attestation_replay_after_tx");
        (void)ledger_tx_window_save_from_dag("attestation_replay_after_tx");
    }

    /* TRANSFER vers nous → confirmer + ACK ESP-NOW + attestation LoRa. */
    if (rx_tx->type == TX_TYPE_TRANSFER &&
        public_key_equal(&rx_tx->to, &s_keypair.public_key)) {

        ret = tx_lifecycle_confirm_received_transfer(&s_dag, &rx_tx->id,
                                                     &s_keypair.public_key);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "TX recue vers nous : confirmation refusee (%d)", ret);
            return;
        }
        persist_runtime_checkpoint("rx_to_me_confirmed");

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
         *
         * [F-LT-001] On verifie d'abord que la HAL LoRa est dispo : sur
         * un device dont l'init SX1262 a echoue (cf. transport_lora.c),
         * l'absence de cette garde provoquait jadis un crash NULL-deref
         * (Guru Meditation) au premier paiement recu. Le LoRa absent
         * n'est pas bloquant pour le paiement direct : l'ACK ESP-NOW
         * (envoye ci-dessus) suffit a confirmer la TX au peer a portee.
         */
        if (transport_lora_available()) {
            signature_t att_sig;
            if (crypto_sign(rx_tx->id.bytes, CRYPTO_HASH_SIZE,
                            &s_keypair, &att_sig) == ESP_OK) {
                uint8_t att_buf[COMM_MSG_ATTESTATION_SIZE];
                size_t att_len = 0;
                if (comm_msg_pack_attestation(att_buf, sizeof(att_buf),
                                              &s_keypair.public_key,
                                              &att_sig, &rx_tx->id,
                                              &att_len) == 0) {
                    comm_msg_attestation_t own_att;
                    memset(&own_att, 0, sizeof(own_att));
                    own_att.attester_key = s_keypair.public_key;
                    own_att.signature = att_sig;
                    own_att.tx_id = rx_tx->id;
                    (void)ledger_attestation_window_add(&own_att);
                    transport_lora_send(att_buf, att_len, "attestation");
                }
            } else {
                ESP_LOGE(TAG, "Echec signature attestation");
            }
        } else {
            ESP_LOGD(TAG, "Attestation LoRa skipee (HAL non dispo)");
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

    esp_err_t ret = tx_lifecycle_confirm_by_ack(&s_dag, &s_lock_table,
                                                tx_id, sender_key);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "ACK recu mais transition refusee (%d)", ret);
        return;
    }
    persist_runtime_checkpoint("ack_confirmed");
    ESP_LOGI(TAG, "TX confirmee (ACK recu et verifie)");
}

void handle_tx_timeout(const comm_event_t *evt)
{
    const hash_t *tx_id = &evt->data.tx_id;
    esp_err_t ret = tx_lifecycle_cancel(&s_dag, &s_lock_table, tx_id,
                                        TX_LIFECYCLE_CANCEL_TIMEOUT);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Timeout TX : annulation refusee (%d)", ret);
        return;
    }
    persist_runtime_checkpoint("tx_timeout");
    ESP_LOGW(TAG, "TX annulee (timeout)");
}

void handle_attestation_received(const comm_event_t *evt)
{
    const comm_msg_attestation_t *att = &evt->data.attestation;

    if (ledger_attestation_window_add(att) != ESP_OK) {
        ESP_LOGW(TAG, "Attestation non persistée");
    }

    const transaction_t *tx = dag_get_by_id(&s_dag, &att->tx_id);
    if (tx == NULL) {
        ESP_LOGD(TAG, "Attestation recue pour TX inconnue, conservee pour replay");
        return;
    }
    if (tx->status == TX_STATUS_CONFIRMED) {
        return;
    }
    if (tx->status == TX_STATUS_CANCELLED) {
        ESP_LOGW(TAG, "Attestation reçue pour TX annulée — ignorée");
        return;
    }
    esp_err_t ret = tx_lifecycle_confirm_by_attestation(
        &s_dag, att, TX_LIFECYCLE_CONFIRM_ATTESTATION);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Attestation rejetee par cycle TX (%d)", ret);
        return;
    }
    persist_runtime_checkpoint("attestation_confirmed");
    (void)ledger_tx_window_save_from_dag("attestation_confirmed");
    ESP_LOGI(TAG, "TX confirmée par attestation LoRa (amount=%"PRIu32")",
             tx->amount);
}
