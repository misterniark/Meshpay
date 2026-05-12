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
     * Compacter le tableau en ne gardant que les TX dont le timestamp
     * est strictement supérieur à before_timestamp.
     * On parcourt le tableau une seule fois en écrasant les TX à supprimer.
     */
    uint32_t write_idx = 0;

    for (uint32_t read_idx = 0; read_idx < dag->count; read_idx++) {
        if (dag->transactions[read_idx].timestamp > before_timestamp) {
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
     */
    return dag->count >= (DAG_MAX_TRANSACTIONS * 4 / 5);
}
