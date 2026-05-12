/**
 * @file dag_validate.c
 * @brief Validation contextuelle des transactions dans le DAG.
 *
 * Vérifie les contraintes qui dépendent de l'état du DAG :
 * - Pas de doublon (transaction déjà présente)
 * - Parents existants dans le DAG
 * - Pas de cycle (une TX ne peut pas être son propre ancêtre)
 */

#include "dag/dag_validate.h"
#include "dag/dag.h"
#include <string.h>

esp_err_t dag_validate_transaction(const dag_t *dag, const transaction_t *tx)
{
    if (dag == NULL || tx == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /* 1. Vérifier que la transaction n'existe pas déjà */
    if (dag_contains(dag, &tx->id)) {
        return ESP_ERR_INVALID_STATE;
    }

    /* 2. Vérifier que tous les parents existent dans le DAG */
    for (uint8_t i = 0; i < tx->parent_count; i++) {
        if (hash_is_zero(&tx->parents[i])) {
            continue;  /* Parent non utilisé */
        }
        if (!dag_contains(dag, &tx->parents[i])) {
            return ESP_ERR_NOT_FOUND;
        }
    }

    /*
     * 3. Vérification d'absence de cycle.
     * Comme la TX n'est pas encore dans le DAG et qu'elle référence
     * des parents existants, il ne peut pas y avoir de cycle tant que
     * la TX ne se référence pas elle-même comme parent.
     */
    for (uint8_t i = 0; i < tx->parent_count; i++) {
        if (hash_equal(&tx->id, &tx->parents[i])) {
            return ESP_ERR_INVALID_STATE;
        }
    }

    return ESP_OK;
}
