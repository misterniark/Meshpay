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
     * 2bis. [F-DG-019] Refuser les parents dupliqués.
     * Si parent_count == 2 et que les deux entrées pointent vers le
     * même hash, la TX référence le même nœud du DAG deux fois — ce
     * n'est pas un cycle au sens strict mais c'est incohérent avec
     * la sémantique "deux parents distincts" attendue par dag_get_tips
     * (qui considère un parent comme "non-tip" et risquerait de le
     * retirer deux fois des candidats). On rejette pour préserver la
     * propreté du graphe.
     */
    if (tx->parent_count == 2 &&
        hash_equal(&tx->parents[0], &tx->parents[1])) {
        return ESP_ERR_INVALID_ARG;
    }

    /*
     * 3. Vérification d'absence de cycle.
     *
     * [F-DG-006 / F-DG-022] Justification précise :
     * On vérifie uniquement la self-loop directe (tx.id == tx.parents[i]).
     * Les cycles indirects de longueur ≥ 2 (A.parents = {B}, B.parents = {A})
     * sont structurellement impossibles dans ce DAG parce que :
     *   - tx.id = SHA-256(payload_signable), où payload_signable inclut
     *     tx.parents. L'id d'une TX dépend donc des id de ses parents.
     *   - Pour créer un cycle A→B→A, il faudrait construire A.id en
     *     connaissant B.id, et B.id en connaissant A.id — ce qui exige
     *     de casser la résistance aux pré-images de SHA-256.
     *
     * La vérification self-loop suffit donc, sous l'hypothèse standard
     * que SHA-256 est non-cassé. Pas besoin d'algorithme BFS/DFS.
     *
     * Au-delà des parents, on contrôle également qu'on ne référence
     * pas une TX dont l'id serait par accident égal au nôtre (collision
     * extrêmement improbable mais détectable à coût nul ici).
     */
    for (uint8_t i = 0; i < tx->parent_count; i++) {
        if (hash_equal(&tx->id, &tx->parents[i])) {
            return ESP_ERR_INVALID_STATE;
        }
    }

    return ESP_OK;
}
