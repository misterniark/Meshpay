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

/*
 * [F-DG-007] Implémentation interne unifiée des deux variantes
 * dag_validate_transaction (strict) et dag_validate_transaction_ext
 * (tolérant aux parents pré-checkpoint).
 *
 * Si `checkpoint_timestamp == 0`, on est en mode strict : tout parent
 * absent du DAG → ESP_ERR_NOT_FOUND. Sinon, un parent absent du DAG
 * est toléré : on présume qu'il a été consolidé dans le checkpoint
 * et purgé. Justification détaillée dans dag_validate.h.
 */
static esp_err_t dag_validate_transaction_impl(const dag_t *dag,
                                                const transaction_t *tx,
                                                uint64_t checkpoint_timestamp)
{
    if (dag == NULL || tx == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /* 1. Vérifier que la transaction n'existe pas déjà */
    if (dag_contains(dag, &tx->id)) {
        return ESP_ERR_INVALID_STATE;
    }

    /*
     * 2. Vérifier que tous les parents existent dans le DAG.
     *
     * [F-DG-007] Si checkpoint_timestamp > 0, on tolère qu'un parent
     * soit absent du DAG : il est alors présumé avoir été consolidé
     * dans le checkpoint et purgé par dag_prune_before. Cette tolérance
     * est nécessaire après un prune (sliding window) car les TX
     * conservées peuvent légitimement référencer des parents anciens.
     *
     * Sécurité : pour les TX locales (chemin d'insertion via
     * dag_insert_and_track), les parents viennent toujours de
     * dag_get_tips qui ne retourne que des TX présentes — un attaquant
     * ne peut donc pas forger un parent "fictif" en exploitant cette
     * tolérance. Pour les TX reçues via merge, dag_merge n'appelle pas
     * du tout dag_validate_transaction* (le rempart est la validation
     * crypto + conflit seq dans dag_merge_transaction).
     */
    for (uint8_t i = 0; i < tx->parent_count; i++) {
        if (hash_is_zero(&tx->parents[i])) {
            continue;  /* Parent non utilisé */
        }
        if (!dag_contains(dag, &tx->parents[i])) {
            if (checkpoint_timestamp > 0) {
                /* Mode tolérant : on présume parent pré-checkpoint. */
                continue;
            }
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

esp_err_t dag_validate_transaction(const dag_t *dag, const transaction_t *tx)
{
    /* Mode strict : checkpoint_timestamp == 0 → exige tous les parents
     * présents dans le DAG. */
    return dag_validate_transaction_impl(dag, tx, 0);
}

esp_err_t dag_validate_transaction_ext(const dag_t *dag,
                                       const transaction_t *tx,
                                       uint64_t checkpoint_timestamp)
{
    return dag_validate_transaction_impl(dag, tx, checkpoint_timestamp);
}
