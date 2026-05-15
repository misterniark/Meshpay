/**
 * @file dag_prune.c
 * @brief Implémentation de l'élagage du DAG.
 *
 * L'élagage supprime les transactions anciennes pour libérer de la
 * mémoire. Les transactions purgées ont été consolidées dans un
 * checkpoint (snapshot de soldes) par le module wallet.
 *
 * Après élagage, le tableau est compacté (pas de trous).
 */

#include "dag/dag_prune.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>

esp_err_t dag_prune_before(dag_t *dag, uint64_t before_timestamp)
{
    if (dag == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTakeRecursive(dag->mutex, portMAX_DELAY);

    /*
     * Compacter le tableau en ne gardant que les TX qui doivent rester.
     *
     * Critère de conservation :
     *  - timestamp strictement supérieur à before_timestamp (TX récente), OU
     *  - statut == LOCKED (paiement en attente d'ACK, [F-DG-005]).
     *
     * [F-DG-005] On ne purge JAMAIS une TX LOCKED, même si son timestamp
     * est ancien. Conserver les LOCKED évite que la lock_table côté
     * core_task devienne orpheline : si une TX LOCKED était prunée,
     * son entrée resterait dans s_lock_table jusqu'au prochain
     * lock_table_expire, qui appellerait dag_set_status sur un hash
     * inexistant (NOT_FOUND silencieusement ignoré), bloquant le
     * montant verrouillé indéfiniment. Pire : si l'attestation LoRa
     * du destinataire arrivait après le prune, dag_get_by_id
     * retournerait NULL et la confirmation serait perdue.
     *
     * Les TX CONFIRMED anciennes ont été consolidées dans le checkpoint
     * (calcul de solde), donc on peut les retirer du DAG. Les CANCELLED
     * anciennes ne sont plus utiles (pas dans le checkpoint, pas dans
     * la lock_table active). Les LOCKED restent jusqu'à expiration ou
     * confirmation (au prochain prune leur timestamp sera toujours
     * "ancien", mais leur statut aura changé, donc elles partiront).
     */
    uint32_t write_idx = 0;

    for (uint32_t read_idx = 0; read_idx < dag->count; read_idx++) {
        const transaction_t *tx = &dag->transactions[read_idx];
        bool keep = (tx->timestamp > before_timestamp) ||
                    (tx->status == TX_STATUS_LOCKED);

        if (keep) {
            /* Garder cette transaction */
            if (write_idx != read_idx) {
                memcpy(&dag->transactions[write_idx],
                       &dag->transactions[read_idx],
                       sizeof(transaction_t));
            }
            write_idx++;
        }
    }

    /* Mettre à zéro les slots libérés (hygiène mémoire) */
    if (write_idx < dag->count) {
        memset(&dag->transactions[write_idx], 0,
               (dag->count - write_idx) * sizeof(transaction_t));
    }

    dag->count = write_idx;

    xSemaphoreGiveRecursive(dag->mutex);
    return ESP_OK;
}

esp_err_t dag_prune_all(dag_t *dag)
{
    if (dag == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTakeRecursive(dag->mutex, portMAX_DELAY);

    memset(dag->transactions, 0, sizeof(dag->transactions));
    dag->count = 0;

    xSemaphoreGiveRecursive(dag->mutex);
    return ESP_OK;
}

bool dag_needs_checkpoint(const dag_t *dag)
{
    if (dag == NULL) return false;

    /*
     * Déclencher le checkpoint à 80% de la capacité au lieu de 100%.
     * Cela laisse une marge de 20% (~50 TX) pour continuer à insérer
     * des transactions pendant que le checkpoint est en cours de
     * création et de sauvegarde. Sans cette marge, le DAG se remplit
     * complètement et les nouvelles TX sont rejetées pendant le
     * processus de checkpoint.
     *
     * [F-DG-023 / F-DG-024] On délègue à dag_count() qui prend le
     * mutex, plutôt que de lire dag->count directement (incohérent
     * avec le reste de l'API qui passe systématiquement par le mutex).
     * Cela ferme le risque qu'un futur caller invoque
     * dag_needs_checkpoint depuis un contexte sans s_state_mutex et
     * lise un dag->count en cours d'écriture par dag_merge_transaction.
     */
    return dag_count(dag) >= (DAG_MAX_TRANSACTIONS * 4 / 5);
}
