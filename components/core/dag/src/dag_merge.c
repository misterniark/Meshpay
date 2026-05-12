/**
 * @file dag_merge.c
 * @brief Implémentation de la fusion de sous-graphes (sync LoRa).
 *
 * Quand un device reçoit des transactions via LoRa, ce module les
 * intègre dans le DAG local. Les doublons sont ignorés, les nouvelles
 * transactions sont insérées.
 *
 * Particularité : contrairement à dag_validate, la fusion accepte les
 * transactions dont les parents ne sont pas encore dans le DAG local.
 * Ces parents arriveront dans une synchronisation ultérieure (convergence
 * éventuelle).
 */

#include "dag/dag_merge.h"
#include "transaction/tx_validate.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>

esp_err_t dag_merge_transaction(dag_t *dag, const transaction_t *tx,
                                const master_keys_t *master_keys,
                                dag_merge_result_t *result)
{
    if (dag == NULL || tx == NULL || result == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTakeRecursive(dag->mutex, portMAX_DELAY);

    /* Si la transaction existe déjà → doublon, on ignore */
    if (dag_contains(dag, &tx->id)) {
        *result = DAG_MERGE_DUPLICATE;
        xSemaphoreGiveRecursive(dag->mutex);
        return ESP_OK;
    }

    /*
     * [I3-fix] Detection de conflits par nonce monotone.
     *
     * Si le DAG contient deja une TX avec meme (from, seq) mais un id
     * different, c'est une double-depense de l'emetteur : il a tente
     * d'emettre deux TX distinctes avec le meme numero de sequence.
     *
     * On rejette la nouvelle TX (la plus ancienne prevaut par ordre
     * d'arrivee dans le DAG). L'emetteur est donc incite a maintenir
     * un compteur strict.
     *
     * Les MINT et TRANSFER partagent le meme espace de seq par emetteur.
     * Le seq=0 est tolere UNIQUEMENT pour la premiere TX d'un emetteur
     * (genesis MINT notamment). Au-dela, chaque TX d'un meme from doit
     * avoir un seq unique.
     */
    for (uint32_t i = 0; i < dag->count; i++) {
        const transaction_t *existing = &dag->transactions[i];
        if (existing->seq == tx->seq &&
            public_key_equal(&existing->from, &tx->from)) {
            *result = DAG_MERGE_CONFLICT;
            xSemaphoreGiveRecursive(dag->mutex);
            return ESP_OK;
        }
    }

    /* Si le DAG est plein → rejeté */
    if (dag->count >= DAG_MAX_TRANSACTIONS) {
        *result = DAG_MERGE_REJECTED;
        xSemaphoreGiveRecursive(dag->mutex);
        return ESP_ERR_NO_MEM;
    }

    /*
     * Validation cryptographique de la transaction reçue via LoRa.
     *
     * Avant d'insérer dans le DAG, on vérifie :
     * 1. La structure (invariants de base : montant > 0, parents, type...)
     * 2. La signature Ed25519 (hash recalculé + vérification signature)
     * 3. Pour les MINT : que le signataire est un maître autorisé
     *
     * Sans ces vérifications, un attaquant pourrait injecter des TX
     * forgées via LoRa (faux MINT, signatures invalides, etc.).
     */

    /* Étape 1 : vérification structurelle */
    if (tx_validate_structure(tx) != ESP_OK) {
        *result = DAG_MERGE_REJECTED;
        xSemaphoreGiveRecursive(dag->mutex);
        return ESP_ERR_INVALID_ARG;
    }

    /* Étape 2 : vérification du hash et de la signature Ed25519 */
    if (tx_validate_signature(tx) != ESP_OK) {
        *result = DAG_MERGE_REJECTED;
        xSemaphoreGiveRecursive(dag->mutex);
        return ESP_ERR_INVALID_ARG;
    }

    /* Étape 3 : pour les MINT, vérifier que le signataire est un maître autorisé */
    if (tx->type == TX_TYPE_MINT && master_keys != NULL) {
        if (tx_validate_master(tx, master_keys) != ESP_OK) {
            *result = DAG_MERGE_REJECTED;
            xSemaphoreGiveRecursive(dag->mutex);
            return ESP_ERR_INVALID_ARG;
        }
    }

    /* Insérer la transaction (les parents manquants ne sont PAS bloquants) */
    memcpy(&dag->transactions[dag->count], tx, sizeof(transaction_t));
    dag->count++;

    *result = DAG_MERGE_INSERTED;
    xSemaphoreGiveRecursive(dag->mutex);
    return ESP_OK;
}

esp_err_t dag_merge_batch(dag_t *dag, const transaction_t *transactions,
                          uint32_t count, const master_keys_t *master_keys,
                          uint32_t *inserted_count)
{
    if (dag == NULL || transactions == NULL || inserted_count == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    *inserted_count = 0;

    for (uint32_t i = 0; i < count; i++) {
        dag_merge_result_t result;
        esp_err_t err = dag_merge_transaction(dag, &transactions[i],
                                              master_keys, &result);

        if (result == DAG_MERGE_INSERTED) {
            (*inserted_count)++;
        }

        /* Si le DAG est plein, on arrête */
        if (err == ESP_ERR_NO_MEM) {
            return ESP_ERR_NO_MEM;
        }
    }

    return ESP_OK;
}
