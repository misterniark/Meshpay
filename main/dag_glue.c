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
#include "persistence/nvs_next_seq.h"
#include "time_glue.h"
#include "wallet/wallet_checkpoint.h"

static const char *TAG = "dag_glue";

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
     * [F-WL-004] Si checkpoint_create échoue à cause d'une TX qui
     * provoque un overflow ou sature CHECKPOINT_MAX_ACCOUNTS, on
     * récupère l'ID de la TX fautive (out param) et on la marque
     * CANCELLED. Le prochain appel pourra alors aboutir.
     */
    hash_t failed_tx_id;
    memset(&failed_tx_id, 0, sizeof(failed_tx_id));
    esp_err_t ret = checkpoint_create_ext(&s_dag, &s_checkpoint,
                                          &s_currency.mint_authorities[0],
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
                esp_err_t cret = dag_set_status(&s_dag, &failed_tx_id,
                                                TX_STATUS_CANCELLED);
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

    memcpy(&s_checkpoint, &new_chk, sizeof(checkpoint_t));
    if (s_checkpoint_save) {
        s_checkpoint_save(&s_checkpoint, NULL);
    }

    /*
     * [F-DG-011] Sauvegarder le max_seq du propriétaire en NVS dédiée.
     * Cette clé est consultée au boot si NVS_KEY_NEXT_SEQ est perdu ET
     * que le DAG est vide (post-prune). Sans cette persistance, le
     * device repartait de seq=0 après corruption NVS+prune complet et
     * était banni par les peers sur conflit de seq.
     */
    nvs_persist_own_max_seq();

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
    return ESP_OK;
}
