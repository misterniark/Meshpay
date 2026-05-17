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
#include "dag/dag_validate.h"
#include "persistence/ledger_store.h"
#include "persistence/nvs_next_seq.h"
#include "time_glue.h"
#include "tx_lifecycle/tx_lifecycle.h"
#include "wallet/wallet_checkpoint.h"

static const char *TAG = "dag_glue";

static const public_key_t *current_fee_recipient(void)
{
    return (s_currency.mint_authority_count > 0)
               ? &s_currency.mint_authorities[0]
               : NULL;
}

static void prune_after_checkpoint_if_needed(uint64_t checkpoint_timestamp)
{
    if (checkpoint_timestamp > 0) {
        dag_prune_before(&s_dag, checkpoint_timestamp);
    } else {
        ESP_LOGW(TAG, "Checkpoint timestamp=0 : prune saute pour eviter "
                      "la suppression silencieuse de TX timestamp=0");
    }
}

static esp_err_t commit_runtime_checkpoint(const checkpoint_t *checkpoint,
                                           const char *reason)
{
    if (checkpoint == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_checkpoint_save) {
        esp_err_t ret = s_checkpoint_save(checkpoint, NULL);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Sauvegarde checkpoint echoue (%s): %s",
                     reason ? reason : "?", esp_err_to_name(ret));
            return ret;
        }
    }

    /*
     * Phase D : la fenetre durable est une fenetre d'historique/reseau,
     * pas un simple miroir du DAG RAM. On la fusionne avant le prune,
     * sinon les TX confirmees juste consolidees disparaitraient de
     * l'historique UI et du gossip LoRa post-reboot.
     */
    esp_err_t ret = ledger_tx_window_save_from_dag(reason);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Sauvegarde fenetre TX echoue (%s): %s",
                 reason ? reason : "?", esp_err_to_name(ret));
        return ret;
    }

    memcpy(&s_checkpoint, checkpoint, sizeof(s_checkpoint));
    prune_after_checkpoint_if_needed(s_checkpoint.timestamp);
    nvs_persist_own_max_seq();

    /*
     * Re-sauver apres prune est peu couteux ici et capture l'etat RAM
     * final tout en conservant les anciennes TX grace a la fusion.
     */
    (void)ledger_tx_window_save_from_dag(reason);
    return ESP_OK;
}

esp_err_t persist_runtime_checkpoint(const char *reason)
{
    static checkpoint_t durable_chk;
    memset(&durable_chk, 0, sizeof(durable_chk));

    esp_err_t ret = checkpoint_create_ext(&s_dag, &s_checkpoint,
                                          current_fee_recipient(),
                                          &durable_chk, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Snapshot checkpoint runtime echoue (%s): %s",
                 reason ? reason : "?", esp_err_to_name(ret));
        return ret;
    }

    ret = commit_runtime_checkpoint(&durable_chk, reason);
    if (ret != ESP_OK) {
        return ret;
    }

    ESP_LOGI(TAG, "Snapshot checkpoint runtime commit (%s, ts=%llu, reste=%"PRIu32" TX)",
             reason ? reason : "?",
             (unsigned long long)durable_chk.timestamp,
             s_dag.count);
    return ESP_OK;
}

void auto_checkpoint_if_needed(void)
{
    if (!dag_needs_checkpoint(&s_dag)) {
        return;
    }

    /*
     * [F-WL-010] `checkpoint_t` fait ~9 Ko (250 entrées) — trop gros
     * pour la stack. Allocation statique : auto_checkpoint_if_needed
     * est appelée sous s_state_mutex, donc l'accès est sérialisé.
     */
    static checkpoint_t new_chk;
    memset(&new_chk, 0, sizeof(new_chk));

    /*
     * [F-MN-009] Sans autorité MINT configurée, le fee_recipient
     * `mint_authorities[0]` n'a aucun sens (lecture OOB potentielle si
     * la config est corrompue avec count=0). On passe NULL pour
     * indiquer "fees brûlés" plutôt que d'accéder à l'index 0 d'un
     * tableau sans contenu garanti.
     */
    /*
     * [F-WL-004] Si checkpoint_create échoue à cause d'une TX qui
     * provoque un overflow ou sature CHECKPOINT_MAX_ACCOUNTS, on
     * récupère l'ID de la TX fautive (out param) et on la marque
     * CANCELLED. Le prochain appel pourra alors aboutir.
     */
    hash_t failed_tx_id;
    memset(&failed_tx_id, 0, sizeof(failed_tx_id));
    esp_err_t ret = checkpoint_create_ext(&s_dag, &s_checkpoint,
                                          current_fee_recipient(),
                                          &new_chk, &failed_tx_id);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Echec creation checkpoint automatique (%d)", ret);
        if (ret == ESP_ERR_INVALID_STATE || ret == ESP_ERR_NO_MEM) {
            /*
             * [F-WL-004] Marquer la TX fautive comme CANCELLED si
             * identifiée. Le checkpoint suivant la sautera et pourra
             * progresser. Sans ce mécanisme, une TX malformée (amount
             * proche de UINT32_MAX, identités > CHECKPOINT_MAX_ACCOUNTS)
             * bloquerait définitivement l'élagage et saturerait le DAG.
             */
            bool zero_id = true;
            for (size_t i = 0; i < sizeof(failed_tx_id); i++) {
                if (failed_tx_id.bytes[i] != 0) { zero_id = false; break; }
            }
            if (!zero_id) {
                esp_err_t cret = tx_lifecycle_cancel(
                    &s_dag, NULL, &failed_tx_id,
                    TX_LIFECYCLE_CANCEL_CHECKPOINT_GUARD);
                if (cret == ESP_OK) {
                    ESP_LOGW(TAG, "TX fautive marquee CANCELLED pour "
                                  "debloquer le checkpoint (cf. F-WL-004)");
                } else {
                    ESP_LOGE(TAG, "Echec marquage CANCELLED (%d) — "
                                  "intervention manuelle requise", cret);
                }
            }
        }
        return;
    }

    /*
     * [F-CU-002] Fonte AUTOMATIQUE DÉSACTIVÉE — décision design 2026-05-16.
     *
     * Le mécanisme de fonte tel qu'implémenté n'était appliqué qu'aux
     * devices `TIME_MODE_MASTER` (cf. condition d'origine). Les acheteurs
     * en mode SLAVE ou sans horloge ne fondaient jamais leurs soldes —
     * violation de la règle monétaire sur la majorité du parc.
     *
     * Plutôt que d'étendre naïvement la fonte à tous les devices avec
     * wall-clock (option (a) du finding), on suspend le déclenchement
     * automatique tant qu'un mécanisme robuste n'est pas conçu :
     *   - quorum d'application synchronisé entre devices,
     *   - garantie de convergence en présence de devices intermittents,
     *   - persistance de last_melt_timestamp cohérente après reboot.
     *
     * Le code des fonctions `currency_melt_*` reste en place et testé
     * pour une réintroduction propre dans un Lot dédié. Le champ
     * `s_checkpoint.last_melt_timestamp` est simplement propagé tel
     * quel sans avancer.
     */
    new_chk.last_melt_timestamp = s_checkpoint.last_melt_timestamp;

    if (commit_runtime_checkpoint(&new_chk, "auto_checkpoint") != ESP_OK) {
        return;
    }

    ESP_LOGI(TAG, "Checkpoint automatique cree + DAG elague "
             "(reste %"PRIu32" TX)", s_dag.count);
}

esp_err_t dag_insert_and_track(const transaction_t *tx)
{
    /*
     * [F-DG-001] Validation contextuelle AVANT insertion.
     *
     * dag_validate_transaction_ext vérifie que :
     *  1. la TX n'existe pas déjà (anti-double-insert),
     *  2. tous les parents référencés sont présents dans le DAG
     *     OU antérieurs au checkpoint (F-DG-007 : tolérance prune),
     *  3. les parents ne sont pas dupliqués (F-DG-019),
     *  4. la TX ne se référence pas elle-même comme parent (anti-cycle).
     *
     * Avant ce fix (F-DG-001), cette fonction était écrite mais JAMAIS
     * appelée en production (seul test_dag.c l'invoquait). Le chemin
     * local (initiate_payment, initiate_mint, attempt_beneficiary_forward)
     * s'appuyait uniquement sur dag_insert qui ne vérifie ni l'existence
     * des parents ni l'absence de cycle.
     *
     * [F-DG-007] On passe s_checkpoint.timestamp en argument pour
     * tolérer les parents pré-checkpoint (TX conservées qui référencent
     * un parent ancien purgé après prune). Si le checkpoint n'a pas
     * encore été créé (s_checkpoint.timestamp == 0), le comportement
     * reste strict.
     *
     * On retourne ESP_ERR_INVALID_ARG pour une TX malformée
     * (parent inconnu, self-loop, parents dupliqués) — cohérent avec
     * le code de retour que dag_insert utilise pour ses propres rejets.
     */
    esp_err_t ret = dag_validate_transaction_ext(&s_dag, tx,
                                                  s_checkpoint.timestamp);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "TX rejetee par dag_validate_transaction_ext: 0x%x", ret);
        return ret;
    }

    ret = dag_insert(&s_dag, tx);
    if (ret != ESP_OK) {
        return ret;
    }
    auto_checkpoint_if_needed();
    (void)ledger_tx_window_save_from_dag("dag_insert");
    return ESP_OK;
}
