/**
 * @file wallet_checkpoint.h
 * @brief Gestion des checkpoints (snapshots de soldes).
 *
 * Un checkpoint est un snapshot des soldes de tous les comptes connus
 * à un instant donné. Il permet d'élaguer le DAG en ne gardant que
 * les transactions postérieures au checkpoint.
 *
 * La persistance (sauvegarde/chargement en Flash) est abstraite via
 * des fonctions injectables pour découpler core/ de hal/.
 */

#ifndef WALLET_CHECKPOINT_H
#define WALLET_CHECKPOINT_H

#include "crypto/crypto_types.h"
#include "dag/dag.h"
#include "esp_err.h"
#include <stdint.h>

/** Nombre maximum de comptes dans un checkpoint */
#define CHECKPOINT_MAX_ACCOUNTS 64

/**
 * @brief Entrée d'un checkpoint : association clé publique → solde.
 */
typedef struct {
    public_key_t key;    /**< Clé publique du compte */
    uint32_t     balance;/**< Solde confirmé au moment du checkpoint */
} checkpoint_entry_t;

/**
 * @brief Structure d'un checkpoint (snapshot des soldes).
 *
 * Contient la liste de tous les comptes avec leur solde confirmé,
 * le hash de la dernière transaction incluse, et le timestamp.
 */
typedef struct {
    checkpoint_entry_t accounts[CHECKPOINT_MAX_ACCOUNTS]; /**< Comptes et soldes */
    uint32_t           account_count;  /**< Nombre de comptes dans le checkpoint */
    hash_t             last_tx_id;     /**< Hash de la dernière TX incluse */
    uint64_t           timestamp;      /**< Timestamp du checkpoint */
    uint64_t           last_melt_timestamp; /**< Timestamp global du dernier traitement de fonte (ms) */
} checkpoint_t;

/**
 * @brief Fonction de sauvegarde d'un checkpoint (injectable).
 *
 * En production : écriture dans le NVS ou SPIFFS.
 * En test : sauvegarde en mémoire.
 *
 * @param checkpoint Checkpoint à sauvegarder
 * @param ctx        Contexte utilisateur (pointeur vers le storage)
 * @return ESP_OK en cas de succès
 */
typedef esp_err_t (*checkpoint_save_fn)(const checkpoint_t *checkpoint, void *ctx);

/**
 * @brief Fonction de chargement d'un checkpoint (injectable).
 *
 * @param checkpoint Checkpoint à remplir
 * @param ctx        Contexte utilisateur
 * @return ESP_OK en cas de succès, ESP_ERR_NOT_FOUND si aucun checkpoint
 */
typedef esp_err_t (*checkpoint_load_fn)(checkpoint_t *checkpoint, void *ctx);

/**
 * @brief Crée un checkpoint à partir de l'état actuel du DAG.
 *
 * Parcourt toutes les transactions confirmées du DAG et calcule le
 * solde de chaque compte impliqué. Le résultat est un snapshot
 * des soldes au moment du checkpoint.
 *
 * Les frais de transfert sont crédités au fee_recipient s'il est
 * fourni (non-NULL et non-zéro). Sinon, ils sont brûlés (détruits,
 * retirés de la masse monétaire).
 *
 * @param[in]  dag            DAG source
 * @param[in]  base           Checkpoint précédent (NULL si premier checkpoint).
 *                            Les soldes du base sont utilisés comme point de départ.
 * @param[in]  fee_recipient  Clé du destinataire des frais (NULL = fees brûlés).
 * @param[out] checkpoint     Checkpoint créé
 * @return ESP_OK en cas de succès
 *         ESP_ERR_NO_MEM si trop de comptes (> CHECKPOINT_MAX_ACCOUNTS)
 */
esp_err_t checkpoint_create(const dag_t *dag, const checkpoint_t *base,
                            const public_key_t *fee_recipient,
                            checkpoint_t *checkpoint);

/**
 * @brief Récupère le solde d'un compte dans un checkpoint.
 *
 * @param[in]  checkpoint Checkpoint source
 * @param[in]  key        Clé publique du compte
 * @param[out] balance    Solde du compte (0 si absent)
 * @return ESP_OK si trouvé, ESP_ERR_NOT_FOUND si le compte n'existe pas
 */
esp_err_t checkpoint_get_balance(const checkpoint_t *checkpoint,
                                 const public_key_t *key,
                                 uint32_t *balance);

#endif /* WALLET_CHECKPOINT_H */
