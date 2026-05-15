/**
 * @file dag_merge.h
 * @brief Fusion de sous-graphes pour la synchronisation LoRa.
 *
 * Quand un device reçoit des transactions via LoRa, il doit les
 * fusionner dans son DAG local. Ce module gère l'insertion de
 * transactions distantes en gérant les doublons et les conflits.
 */

#ifndef DAG_MERGE_H
#define DAG_MERGE_H

#include "dag/dag.h"
#include "transaction/tx_types.h"
#include "transaction/tx_validate.h"
#include "esp_err.h"

/**
 * @brief Résultat de la fusion d'une transaction distante.
 */
typedef enum {
    DAG_MERGE_INSERTED = 0,  /**< Transaction nouvelle, insérée avec succès */
    DAG_MERGE_DUPLICATE = 1, /**< Transaction déjà présente, ignorée */
    DAG_MERGE_CONFLICT = 2,  /**< Conflit détecté (même émetteur, même parents, montants différents) */
    DAG_MERGE_REJECTED = 3,  /**< Transaction rejetée (DAG plein ou invalide) */
} dag_merge_result_t;

/**
 * @brief Fusionne une transaction distante dans le DAG local.
 *
 * Comportement :
 * - Si la transaction existe déjà (même hash) → DUPLICATE (ignorée)
 * - Si c'est une nouvelle transaction valide → INSERTED
 * - Si le DAG est plein → REJECTED
 *
 * Les parents manquants ne sont PAS bloquants : la transaction
 * est insérée même si ses parents ne sont pas encore dans le DAG
 * local (ils arriveront dans une sync ultérieure).
 *
 * @param[in,out] dag         DAG local
 * @param[in]     tx          Transaction distante à fusionner
 * @param[in]     master_keys Liste des clés maîtres autorisées pour les MINT.
 *                            [F-DG-002] Si NULL, tous les MINT sont REJETÉS
 *                            (l'ancien comportement "accepter sans vérif"
 *                            ouvrait une faille d'authentification). Pour les
 *                            tests qui ne mergent que des TRANSFER, passer
 *                            NULL reste valide — les MINT seront rejetés.
 * @param[out]    result      Résultat de la fusion
 * @return ESP_OK en cas de succès (même si DUPLICATE ou REJECTED)
 *         ESP_ERR_NO_MEM si le DAG est plein (REJECTED)
 *         ESP_ERR_INVALID_ARG si la validation échoue (REJECTED)
 */
esp_err_t dag_merge_transaction(dag_t *dag, const transaction_t *tx,
                                const master_keys_t *master_keys,
                                dag_merge_result_t *result);

/**
 * @brief Fusionne un lot de transactions distantes dans le DAG local.
 *
 * Insère chaque transaction du tableau, dans l'ordre. Les doublons
 * sont ignorés silencieusement.
 *
 * @param[in,out] dag             DAG local
 * @param[in]     transactions    Tableau de transactions distantes
 * @param[in]     count           Nombre de transactions dans le tableau
 * @param[in]     master_keys     Liste des clés maîtres autorisées pour les MINT
 * @param[out]    inserted_count  Nombre de transactions effectivement insérées
 * @return ESP_OK si toutes les transactions ont été traitées
 *         ESP_ERR_NO_MEM si le DAG est devenu plein avant la fin
 */
esp_err_t dag_merge_batch(dag_t *dag, const transaction_t *transactions,
                          uint32_t count, const master_keys_t *master_keys,
                          uint32_t *inserted_count);

#endif /* DAG_MERGE_H */
