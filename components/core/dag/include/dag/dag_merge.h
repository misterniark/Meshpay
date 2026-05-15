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
 * [F-DG-009] CETTE FONCTION EST DÉLIBÉRÉMENT NON-ATOMIQUE.
 *
 * Si une transaction du lot échoue (signature invalide, conflit de
 * seq, DAG plein), les transactions précédentes du lot ont déjà été
 * insérées dans le DAG et ne sont PAS retirées. C'est cohérent avec
 * le modèle de convergence éventuelle du ledger : un fragment de
 * synchronisation partiellement appliqué reste valide, les TX
 * manquantes arriveront lors d'une synchronisation ultérieure.
 *
 * Conséquences pour l'appelant :
 *   - `*inserted_count` reflète exactement le nombre de TX insérées.
 *   - Sur retour `ESP_ERR_NO_MEM` : la 1ère TX qui n'a pas tenu est
 *     `transactions[*inserted_count]` ; les suivantes n'ont pas été
 *     tentées. Les `*inserted_count` premières sont en place.
 *   - Sur retour `ESP_OK` : chaque TX a été tentée individuellement.
 *     Certaines peuvent avoir été rejetées (signature, seq, etc.) ;
 *     `inserted_count < count` signale ce cas. Les logs `ESP_LOGW`
 *     dans dag_merge.c précisent la raison de chaque rejet.
 *
 * Aucun rollback n'est implémenté. Si une atomicité forte est requise
 * (par exemple pour une consensus engine externe), il faut soit
 * pré-valider tout le lot avant d'appeler cette fonction, soit
 * encapsuler dans une couche d'ordonnancement applicatif.
 *
 * @param[in,out] dag             DAG local
 * @param[in]     transactions    Tableau de transactions distantes
 * @param[in]     count           Nombre de transactions dans le tableau
 * @param[in]     master_keys     Liste des clés maîtres autorisées pour les MINT
 * @param[out]    inserted_count  Nombre de transactions effectivement insérées
 * @return ESP_OK si toutes les transactions ont été traitées (qu'elles
 *                soient insérées, dédoublonnées ou rejetées
 *                individuellement)
 *         ESP_ERR_NO_MEM si le DAG est devenu plein avant la fin
 */
esp_err_t dag_merge_batch(dag_t *dag, const transaction_t *transactions,
                          uint32_t count, const master_keys_t *master_keys,
                          uint32_t *inserted_count);

#endif /* DAG_MERGE_H */
