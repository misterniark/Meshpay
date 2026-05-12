/**
 * @file wallet_lock.h
 * @brief Gestion du verrouillage des fonds (modèle "solde verrouillé").
 *
 * Quand un paiement est initié, le montant est verrouillé (status LOCKED).
 * Le verrouillage a un timeout de 30 secondes. Si le destinataire ne
 * confirme pas avant le timeout, la transaction est annulée et le
 * montant est déverrouillé.
 *
 * Ce module gère la table des verrous en cours et l'expiration automatique.
 */

#ifndef WALLET_LOCK_H
#define WALLET_LOCK_H

#include "wallet/wallet.h"
#include "crypto/crypto_types.h"
#include "esp_err.h"

/** Durée du timeout de verrouillage en millisecondes (30 secondes) */
#define WALLET_LOCK_TIMEOUT_MS 30000

/** Nombre maximum de verrous simultanés */
#define WALLET_MAX_LOCKS 10

/**
 * @brief Entrée dans la table des verrous.
 *
 * Chaque verrou correspond à une transaction LOCKED en attente de
 * confirmation ou d'annulation.
 */
typedef struct {
    hash_t   tx_id;         /**< ID de la transaction verrouillée */
    uint32_t amount;        /**< Montant verrouillé */
    uint64_t lock_time;     /**< Timestamp de création du verrou (ms) */
    bool     active;        /**< true si le verrou est actif */
} lock_entry_t;

/**
 * @brief Table des verrous en cours.
 */
typedef struct {
    lock_entry_t entries[WALLET_MAX_LOCKS]; /**< Entrées de la table */
    wallet_t    *wallet;                     /**< Référence au wallet (pour get_time) */
} lock_table_t;

/**
 * @brief Initialise la table des verrous.
 *
 * @param[out] table  Table à initialiser
 * @param[in]  wallet Référence au wallet (pour accéder à get_time)
 * @return ESP_OK en cas de succès
 */
esp_err_t lock_table_init(lock_table_t *table, wallet_t *wallet);

/**
 * @brief Verrouille un montant pour une transaction.
 *
 * Crée une entrée dans la table des verrous. Le montant est considéré
 * comme dépensé dans le calcul du solde disponible.
 *
 * @param[in,out] table  Table des verrous
 * @param[in]     tx_id  ID de la transaction verrouillée
 * @param[in]     amount Montant à verrouiller
 * @return ESP_OK en cas de succès
 *         ESP_ERR_NO_MEM si la table est pleine
 */
esp_err_t lock_table_lock(lock_table_t *table, const hash_t *tx_id, uint32_t amount);

/**
 * @brief Confirme un verrou (le paiement a été accepté).
 *
 * Supprime l'entrée de la table des verrous. Le montant reste dépensé
 * (la transaction passe de LOCKED à CONFIRMED dans le DAG).
 *
 * @param[in,out] table Table des verrous
 * @param[in]     tx_id ID de la transaction à confirmer
 * @return ESP_OK si le verrou a été trouvé et supprimé
 *         ESP_ERR_NOT_FOUND si le verrou n'existe pas
 */
esp_err_t lock_table_confirm(lock_table_t *table, const hash_t *tx_id);

/**
 * @brief Annule un verrou (timeout ou rejet).
 *
 * Supprime l'entrée et libère le montant (redevient disponible).
 * La transaction correspondante doit être passée à CANCELLED dans le DAG.
 *
 * @param[in,out] table Table des verrous
 * @param[in]     tx_id ID de la transaction à annuler
 * @return ESP_OK si le verrou a été trouvé et annulé
 *         ESP_ERR_NOT_FOUND si le verrou n'existe pas
 */
esp_err_t lock_table_cancel(lock_table_t *table, const hash_t *tx_id);

/**
 * @brief Vérifie et expire les verrous dépassant le timeout.
 *
 * Parcourt la table et annule automatiquement les verrous dont
 * le timestamp de création + WALLET_LOCK_TIMEOUT_MS < temps courant.
 * Les tx_id des verrous expirés sont copiés dans expired_ids pour
 * permettre au core_task de marquer les TX comme CANCELLED dans le DAG.
 *
 * @param[in,out] table           Table des verrous
 * @param[out]    expired_ids     Tableau de sortie pour les tx_id expirés (peut être NULL)
 * @param[in]     max_expired     Taille du tableau expired_ids (ignoré si expired_ids == NULL)
 * @param[out]    expired_count   Nombre de verrous expirés (peut être NULL)
 * @return ESP_OK en cas de succès
 */
esp_err_t lock_table_expire(lock_table_t *table, hash_t *expired_ids,
                            uint32_t max_expired, uint32_t *expired_count);

/**
 * @brief Calcule le montant total actuellement verrouillé.
 *
 * @param[in]  table  Table des verrous
 * @param[out] total  Montant total verrouillé
 * @return ESP_OK en cas de succès
 */
esp_err_t lock_table_total_locked(const lock_table_t *table, uint32_t *total);

#endif /* WALLET_LOCK_H */
