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
 * 3. La transaction ne crée pas de cycle (un noeud ne peut pas être
 *    son propre ancêtre)
 *
 * Cette validation est complémentaire à tx_validate_structure et
 * tx_validate_signature qui vérifient la transaction isolément.
 *
 * @param[in] dag DAG de référence
 * @param[in] tx  Transaction à valider
 * @return ESP_OK si la transaction est valide dans le contexte du DAG
 *         ESP_ERR_INVALID_STATE si la transaction existe déjà
 *         ESP_ERR_NOT_FOUND si un parent n'existe pas dans le DAG
 */
esp_err_t dag_validate_transaction(const dag_t *dag, const transaction_t *tx);

#endif /* DAG_VALIDATE_H */
