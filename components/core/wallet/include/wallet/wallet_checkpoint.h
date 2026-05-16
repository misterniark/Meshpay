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

/**
 * Nombre maximum de comptes dans un checkpoint.
 *
 * [F-WL-010] Dimensionné pour absorber un DAG plein de TX impliquant
 * des identités distinctes, soit `DAG_MAX_TRANSACTIONS = 250`. Au-delà,
 * `checkpoint_create` retourne `ESP_ERR_NO_MEM` et le DAG ne peut plus
 * être élagué — d'où la nécessité d'aligner les deux capacités.
 *
 * Coût mémoire : 250 × (32 + 4) octets ≈ 9 Ko par checkpoint, stocké
 * en BSS dans `s_checkpoint`. NVS supporte des blobs de cette taille
 * via chunking interne transparent.
 */
#define CHECKPOINT_MAX_ACCOUNTS 250

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
 * [F-WL-001] Acquiert le mutex récursif du DAG pendant toute la durée
 * du parcours pour garantir l'atomicité vs un `dag_merge_transaction`
 * concurrent (tâche LoRa). Le mutex étant récursif, l'appel reste sûr
 * même si l'appelant le détient déjà.
 *
 * [F-WL-006] Les `accounts[]` sont triés par clé publique (ordre
 * lexicographique) après construction, pour produire un checkpoint
 * structurellement déterministe entre devices. Préparation pour un
 * éventuel mécanisme de consensus inter-pairs.
 *
 * [F-WL-007] `last_melt_timestamp` est propagé depuis `base` vers le
 * nouveau checkpoint. Le mécanisme de fonte est actuellement désactivé
 * (cf. F-CU-002 dans dag_glue.c) mais le champ est conservé pour une
 * future réactivation.
 *
 * [F-WL-004] En cas d'échec (`ESP_ERR_NO_MEM` ou `ESP_ERR_INVALID_STATE`),
 * `out_failed_tx_id` est rempli avec l'ID de la TX qui a provoqué l'échec
 * pour permettre à l'appelant de la marquer CANCELLED et débloquer
 * l'élagage. Si NULL, l'identifiant n'est pas remonté.
 *
 * @param[in]  dag              DAG source
 * @param[in]  base             Checkpoint précédent (NULL si premier checkpoint).
 *                              Les soldes du base sont utilisés comme point de départ.
 * @param[in]  fee_recipient    Clé du destinataire des frais (NULL = fees brûlés).
 * @param[out] checkpoint       Checkpoint créé
 * @param[out] out_failed_tx_id ID de la TX fautive en cas d'échec (peut être NULL).
 *                              Remplie uniquement si retour != ESP_OK et que la
 *                              cause est identifiable (overflow, saturation).
 * @return ESP_OK en cas de succès
 *         ESP_ERR_NO_MEM si trop de comptes (> CHECKPOINT_MAX_ACCOUNTS)
 *         ESP_ERR_INVALID_STATE si une TX provoque un overflow de solde
 */
esp_err_t checkpoint_create(const dag_t *dag, const checkpoint_t *base,
                            const public_key_t *fee_recipient,
                            checkpoint_t *checkpoint);

/**
 * @brief Variante étendue de `checkpoint_create` qui remonte l'ID de
 *        la TX fautive en cas d'échec.
 *
 * [F-WL-004] Permet à l'appelant (`auto_checkpoint_if_needed`) de
 * marquer la TX problématique comme CANCELLED pour débloquer
 * l'élagage. La sémantique est identique à `checkpoint_create` ;
 * `out_failed_tx_id` peut être NULL pour ignorer cette information.
 */
esp_err_t checkpoint_create_ext(const dag_t *dag, const checkpoint_t *base,
                                const public_key_t *fee_recipient,
                                checkpoint_t *checkpoint,
                                hash_t *out_failed_tx_id);

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
