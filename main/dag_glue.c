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
     *
     * [F-DG-013] Garde-fou timestamp > 0 : checkpoint_create initialise
     * latest_timestamp a 0 et ne le met a jour que pour les TX
     * CONFIRMED rencontrees. Si aucune TX CONFIRMED n'existe (DAG plein
     * uniquement de LOCKED/CANCELLED ou device sans source de temps),
     * new_chk.timestamp = 0 et dag_prune_before(_, 0) supprimerait toute
     * TX dont le timestamp est 0 — destruction silencieuse difficile a
     * diagnostiquer. Si timestamp == 0, on ne prune pas : le checkpoint
     * suivant declenchera une vraie reduction quand des CONFIRMED
     * arriveront.
     */
    if (new_chk.timestamp > 0) {
        dag_prune_before(&s_dag, new_chk.timestamp);
    } else {
        ESP_LOGW(TAG, "Checkpoint timestamp=0 : prune saute pour eviter "
                      "la suppression silencieuse de TX timestamp=0");
    }

    ESP_LOGI(TAG, "Checkpoint automatique cree + DAG elague "
             "(reste %"PRIu32" TX)", s_dag.count);
}

esp_err_t dag_insert_and_track(const transaction_t *tx)
{
    /*
     * [F-DG-001] Validation contextuelle AVANT insertion.
     *
     * dag_validate_transaction vérifie que :
     *  1. la TX n'existe pas déjà (anti-double-insert),
     *  2. tous les parents référencés sont présents dans le DAG,
     *  3. la TX ne se référence pas elle-même comme parent (anti-cycle).
     *
     * Avant ce fix, cette fonction était écrite mais JAMAIS appelée en
     * production (seul test_dag.c l'invoquait). Le chemin local
     * (initiate_payment, initiate_mint, attempt_beneficiary_forward)
     * s'appuyait uniquement sur dag_insert qui ne vérifie ni l'existence
     * des parents ni l'absence de cycle. Le risque pratique était
     * faible parce que les parents viennent de dag_get_tips, mais
     * aucun filet n'existait en cas de bug de sélection des tips ou
     * d'appel direct à dag_insert depuis du code futur.
     *
     * On retourne ESP_ERR_INVALID_ARG pour une TX malformée
     * (parent inconnu, self-loop) — cohérent avec le code de retour
     * que dag_insert utilise pour ses propres rejets.
     */
    esp_err_t ret = dag_validate_transaction(&s_dag, tx);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "TX rejetee par dag_validate_transaction: 0x%x", ret);
        return ret;
    }

    ret = dag_insert(&s_dag, tx);
    if (ret != ESP_OK) {
        return ret;
    }
    auto_checkpoint_if_needed();
    return ESP_OK;
}
