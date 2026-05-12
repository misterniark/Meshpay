/**
 * @file wallet_lock.c
 * @brief Implémentation de la table de verrouillage des fonds.
 *
 * La table des verrous est un tableau statique de WALLET_MAX_LOCKS entrées.
 * Chaque entrée contient l'ID de la transaction, le montant verrouillé,
 * et le timestamp de création.
 *
 * L'expiration est vérifiée périodiquement via lock_table_expire().
 */

#include "wallet/wallet_lock.h"
#include <string.h>

esp_err_t lock_table_init(lock_table_t *table, wallet_t *wallet)
{
    if (table == NULL || wallet == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(table->entries, 0, sizeof(table->entries));
    table->wallet = wallet;

    return ESP_OK;
}

esp_err_t lock_table_lock(lock_table_t *table, const hash_t *tx_id, uint32_t amount)
{
    if (table == NULL || tx_id == NULL || amount == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Chercher un slot libre dans la table */
    for (int i = 0; i < WALLET_MAX_LOCKS; i++) {
        if (!table->entries[i].active) {
            memcpy(&table->entries[i].tx_id, tx_id, sizeof(hash_t));
            table->entries[i].amount = amount;
            table->entries[i].lock_time = table->wallet->get_time();
            table->entries[i].active = true;
            return ESP_OK;
        }
    }

    /* Table pleine */
    return ESP_ERR_NO_MEM;
}

esp_err_t lock_table_confirm(lock_table_t *table, const hash_t *tx_id)
{
    if (table == NULL || tx_id == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Rechercher le verrou par tx_id */
    for (int i = 0; i < WALLET_MAX_LOCKS; i++) {
        if (table->entries[i].active &&
            hash_equal(&table->entries[i].tx_id, tx_id)) {
            /* Supprimer le verrou (le montant reste dépensé dans le DAG) */
            memset(&table->entries[i], 0, sizeof(lock_entry_t));
            return ESP_OK;
        }
    }

    return ESP_ERR_NOT_FOUND;
}

esp_err_t lock_table_cancel(lock_table_t *table, const hash_t *tx_id)
{
    if (table == NULL || tx_id == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Rechercher le verrou par tx_id */
    for (int i = 0; i < WALLET_MAX_LOCKS; i++) {
        if (table->entries[i].active &&
            hash_equal(&table->entries[i].tx_id, tx_id)) {
            /* Supprimer le verrou (le montant redevient disponible) */
            memset(&table->entries[i], 0, sizeof(lock_entry_t));
            return ESP_OK;
        }
    }

    return ESP_ERR_NOT_FOUND;
}

esp_err_t lock_table_expire(lock_table_t *table, hash_t *expired_ids,
                            uint32_t max_expired, uint32_t *expired_count)
{
    if (table == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    uint32_t count = 0;
    uint64_t now = table->wallet->get_time();

    for (int i = 0; i < WALLET_MAX_LOCKS; i++) {
        if (table->entries[i].active) {
            /* Vérifier si le timeout est dépassé */
            if (now - table->entries[i].lock_time >= WALLET_LOCK_TIMEOUT_MS) {

                /*
                 * [M1] Si un buffer de sortie est fourni et qu'il est plein,
                 * on arrête le traitement SANS supprimer l'entrée de la table.
                 * L'appelant devra rappeler cette fonction en boucle pour
                 * traiter les expirations restantes.
                 */
                if (expired_ids != NULL && count >= max_expired) {
                    break;
                }

                /* Copier le tx_id avant de supprimer l'entrée */
                if (expired_ids != NULL) {
                    memcpy(&expired_ids[count], &table->entries[i].tx_id, sizeof(hash_t));
                }
                memset(&table->entries[i], 0, sizeof(lock_entry_t));
                count++;
            }
        }
    }

    if (expired_count != NULL) {
        *expired_count = count;
    }

    return ESP_OK;
}

esp_err_t lock_table_total_locked(const lock_table_t *table, uint32_t *total)
{
    if (table == NULL || total == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /*
     * [H2] Utiliser un accumulateur 64 bits pour éviter l'overflow lors
     * de la somme des montants verrouillés. On vérifie ensuite que le
     * résultat tient dans un uint32_t avant de l'affecter au paramètre
     * de sortie.
     */
    uint64_t sum = 0;
    for (int i = 0; i < WALLET_MAX_LOCKS; i++) {
        if (table->entries[i].active) {
            sum += table->entries[i].amount;
        }
    }

    /* Vérifier que la somme ne dépasse pas la capacité d'un uint32_t */
    if (sum > UINT32_MAX) {
        *total = UINT32_MAX;
        return ESP_ERR_INVALID_STATE;
    }

    *total = (uint32_t)sum;
    return ESP_OK;
}
