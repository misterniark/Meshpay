/**
 * @file dag.h
 * @brief Structure de données du Directed Acyclic Graph (DAG).
 *
 * Le DAG est la structure centrale du ledger. Chaque transaction est un
 * noeud qui référence 1 ou 2 transactions parentes via leurs hashes.
 *
 * Implémentation : tableau statique de 500 transactions max (fenêtre
 * glissante). La recherche par hash est linéaire O(n) — acceptable
 * pour 500 éléments sur ESP32. Optimisable en hashmap si nécessaire.
 *
 * Les "tips" sont les transactions non référencées comme parents par
 * d'autres transactions (les feuilles du DAG). Ce sont les candidats
 * pour devenir parents de nouvelles transactions.
 */

#ifndef DAG_H
#define DAG_H

#include "transaction/tx_types.h"
#include "crypto/crypto_types.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <stdbool.h>

/**
 * Nombre maximum de transactions dans la fenêtre glissante du DAG.
 *
 * Réduit de 500 à 250 pour tenir dans la DRAM de l'ESP32 (180 KB)
 * avec LVGL + WiFi + LoRa. 250 TX = ~57 KB de BSS.
 */
#define DAG_MAX_TRANSACTIONS 250

/** Nombre maximum de tips retournées par dag_get_tips */
#define DAG_MAX_TIPS 32

/**
 * @brief Structure du DAG (Directed Acyclic Graph).
 *
 * Stocke les transactions dans un tableau statique. L'index
 * d'insertion avance de manière circulaire, mais l'élagage
 * est géré par le module wallet (checkpoints).
 */
typedef struct {
    transaction_t transactions[DAG_MAX_TRANSACTIONS]; /**< Tableau des transactions */
    uint32_t count;                                    /**< Nombre de transactions actuellement stockées */
    SemaphoreHandle_t mutex;                           /**< Mutex pour protéger les accès concurrents */
} dag_t;

/**
 * @brief Initialise un DAG vide.
 *
 * Remet à zéro le tableau et le compteur. Doit être appelé
 * avant toute utilisation du DAG.
 *
 * @param[out] dag Structure DAG à initialiser
 * @return ESP_OK en cas de succès
 */
esp_err_t dag_init(dag_t *dag);

/**
 * @brief Insère une transaction dans le DAG.
 *
 * Vérifie que :
 * - Le DAG n'est pas plein (< DAG_MAX_TRANSACTIONS)
 * - La transaction n'existe pas déjà (pas de doublon)
 *
 * Ne vérifie PAS la validité des parents (voir dag_validate).
 * La transaction est copiée dans le DAG (pas de référence).
 *
 * @param[in,out] dag DAG cible
 * @param[in]     tx  Transaction à insérer
 * @return ESP_OK en cas de succès
 *         ESP_ERR_NO_MEM si le DAG est plein
 *         ESP_ERR_INVALID_STATE si la transaction existe déjà
 */
esp_err_t dag_insert(dag_t *dag, const transaction_t *tx);

/**
 * @brief Recherche une transaction par son hash (id).
 *
 * Recherche linéaire O(n) dans le tableau.
 *
 * @param[in]  dag DAG source
 * @param[in]  id  Hash de la transaction recherchée
 * @return Pointeur vers la transaction si trouvée, NULL sinon.
 *         Le pointeur est valide tant que le DAG n'est pas modifié.
 */
const transaction_t *dag_get_by_id(const dag_t *dag, const hash_t *id);

/**
 * @brief Récupère les tips du DAG (transactions non référencées comme parents).
 *
 * Les tips sont les feuilles du graphe : aucune autre transaction ne les
 * référence comme parent. Ce sont les candidats naturels pour devenir
 * parents de la prochaine transaction.
 *
 * @param[in]  dag       DAG source
 * @param[out] tips      Tableau de pointeurs vers les tips
 * @param[in]  max_tips  Taille max du tableau de sortie
 * @param[out] tip_count Nombre de tips trouvées
 * @return ESP_OK en cas de succès
 */
esp_err_t dag_get_tips(const dag_t *dag, const transaction_t **tips,
                       uint32_t max_tips, uint32_t *tip_count);

/**
 * @brief Retourne le nombre de transactions dans le DAG.
 *
 * @param[in] dag DAG source
 * @return Nombre de transactions stockées
 */
uint32_t dag_count(const dag_t *dag);

/**
 * @brief Vérifie si une transaction existe dans le DAG.
 *
 * @param[in] dag DAG source
 * @param[in] id  Hash de la transaction
 * @return true si la transaction existe dans le DAG
 */
bool dag_contains(const dag_t *dag, const hash_t *id);

/**
 * @brief Modifie le statut d'une transaction dans le DAG.
 *
 * Permet de faire passer une TX de LOCKED à CONFIRMED ou CANCELLED.
 * Ne modifie pas le hash ni la signature — seul le champ status
 * (non signé) est affecté.
 *
 * @param[in,out] dag    DAG cible
 * @param[in]     id     Hash de la transaction à modifier
 * @param[in]     status Nouveau statut
 * @return ESP_OK si trouvée et modifiée
 *         ESP_ERR_NOT_FOUND si la transaction n'existe pas
 */
esp_err_t dag_set_status(dag_t *dag, const hash_t *id, tx_status_t status);

#endif /* DAG_H */
