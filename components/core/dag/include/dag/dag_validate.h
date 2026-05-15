/**
 * @file dag_validate.h
 * @brief Validation d'une transaction dans le contexte du DAG.
 *
 * Vérifie les contraintes contextuelles que tx_validate ne peut pas
 * vérifier seul : existence des parents dans le DAG, absence de cycles,
 * absence de doublons.
 */

#ifndef DAG_VALIDATE_H
#define DAG_VALIDATE_H

#include "dag/dag.h"
#include "transaction/tx_types.h"
#include "esp_err.h"

/**
 * @brief Valide une transaction dans le contexte du DAG.
 *
 * Vérifie que :
 * 1. La transaction n'existe pas déjà dans le DAG (pas de double-insert)
 * 2. Tous les parents référencés existent dans le DAG
 *    (sauf parents pré-checkpoint, cf. dag_validate_transaction_ext)
 * 3. La transaction ne crée pas de cycle (un noeud ne peut pas être
 *    son propre ancêtre)
 * 4. La transaction ne référence pas deux fois le même parent
 *    (parents[0] != parents[1] quand parent_count == 2)
 *
 * Cette validation est complémentaire à tx_validate_structure et
 * tx_validate_signature qui vérifient la transaction isolément.
 *
 * Cette version stricte exige que TOUS les parents soient présents
 * dans le DAG. Pour le cas post-checkpoint où certains parents ont
 * été prunés mais sont valides via le checkpoint, utiliser
 * dag_validate_transaction_ext.
 *
 * @param[in] dag DAG de référence
 * @param[in] tx  Transaction à valider
 * @return ESP_OK si la transaction est valide dans le contexte du DAG
 *         ESP_ERR_INVALID_STATE si la transaction existe déjà ou contient un cycle
 *         ESP_ERR_NOT_FOUND si un parent n'existe pas dans le DAG
 *         ESP_ERR_INVALID_ARG si parents dupliqués (F-DG-019)
 */
esp_err_t dag_validate_transaction(const dag_t *dag, const transaction_t *tx);

/**
 * [F-DG-007] Variante "tolérante au prune" de dag_validate_transaction.
 *
 * Identique à dag_validate_transaction sauf que la vérification de
 * présence des parents accepte les parents absents du DAG SI leur
 * référence pointe vers une TX antérieure au checkpoint
 * (`checkpoint_timestamp` est le timestamp du dernier checkpoint
 * connu de l'appelant).
 *
 * Justification : après un `dag_prune_before`, les TX consolidées
 * dans le checkpoint sont retirées du DAG, mais les TX récentes
 * conservées peuvent légitimement référencer ces anciennes TX comme
 * parents. Sans cette tolérance, toute insertion référençant un
 * parent pré-checkpoint serait rejetée à tort.
 *
 * La fonction ne peut pas savoir si la référence est "réelle" (parent
 * pré-checkpoint qui existait vraiment) ou "fictive" (parent forgé
 * qui n'a jamais existé). Pour les TX locales (chemin d'insertion
 * via dag_insert_and_track), les parents viennent de dag_get_tips
 * qui ne retourne que des TX présentes, donc impossible de forger
 * un parent fictif. Pour les TX reçues via merge, dag_merge n'appelle
 * pas du tout dag_validate_transaction (la validation crypto +
 * conflit seq dans dag_merge_transaction est le rempart suffisant).
 *
 * @param[in] dag                 DAG de référence
 * @param[in] tx                  Transaction à valider
 * @param[in] checkpoint_timestamp Timestamp du dernier checkpoint connu.
 *                                Si 0, comportement identique à
 *                                dag_validate_transaction (strict).
 * @return Mêmes codes que dag_validate_transaction
 */
esp_err_t dag_validate_transaction_ext(const dag_t *dag,
                                       const transaction_t *tx,
                                       uint64_t checkpoint_timestamp);

#endif /* DAG_VALIDATE_H */
