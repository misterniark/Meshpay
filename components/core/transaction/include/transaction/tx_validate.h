/**
 * @file tx_validate.h
 * @brief Validation des transactions.
 *
 * Fournit les fonctions pour vérifier la validité d'une transaction :
 * - Structure correcte (champs obligatoires présents)
 * - Signature valide (correspond au contenu et à la clé publique)
 * - Hash (id) correct (correspond au contenu sérialisé)
 * - Autorisation MINT (clé maître autorisée)
 */

#ifndef TX_VALIDATE_H
#define TX_VALIDATE_H

#include "transaction/tx_types.h"
#include "crypto/crypto_types.h"
#include "esp_err.h"

/**
 * @brief Liste des clés publiques des devices maîtres autorisés.
 *
 * Les transactions MINT ne sont valides que si elles sont signées
 * par une clé présente dans cette liste. La liste est injectable
 * pour faciliter les tests et la mise à jour dynamique.
 */
typedef struct {
    const public_key_t *keys;  /**< Tableau de clés publiques autorisées */
    uint8_t count;             /**< Nombre de clés dans le tableau */
} master_keys_t;

/**
 * @brief Vérifie la structure d'une transaction.
 *
 * Contrôle les invariants structurels :
 * - amount > 0
 * - parent_count entre 1 et TX_MAX_PARENTS
 * - au moins un parent non-nul
 * - type valide (TRANSFER ou MINT)
 * - MINT → from doit être nul / TRANSFER → from doit être non-nul
 *
 * Ne vérifie PAS la signature ni le hash (voir tx_validate_signature).
 *
 * @param[in] tx Transaction à vérifier
 * @return ESP_OK si la structure est valide
 *         ESP_ERR_INVALID_ARG si un invariant est violé
 */
esp_err_t tx_validate_structure(const transaction_t *tx);

/**
 * @brief Vérifie la signature et le hash d'une transaction.
 *
 * Recalcule le contenu sérialisé "signable", vérifie que :
 * 1. Le hash SHA-256 correspond au champ id
 * 2. La signature Ed25519 est valide pour la clé publique "from"
 *    (ou la clé de signature pour les MINT)
 *
 * @param[in] tx Transaction à vérifier
 * @return ESP_OK si signature et hash sont valides
 *         ESP_ERR_INVALID_STATE si la signature ou le hash est invalide
 */
esp_err_t tx_validate_signature(const transaction_t *tx);

/**
 * @brief Vérifie qu'une transaction MINT provient d'un device maître autorisé.
 *
 * Compare la clé publique du signataire avec la liste des clés maîtres.
 * Pour les transactions TRANSFER, cette fonction retourne toujours ESP_OK
 * (pas de vérification nécessaire).
 *
 * @param[in] tx          Transaction à vérifier
 * @param[in] master_keys Liste des clés maîtres autorisées
 * @return ESP_OK si autorisé (ou si c'est un TRANSFER)
 *         ESP_ERR_INVALID_STATE si la clé n'est pas dans la liste des maîtres
 */
esp_err_t tx_validate_master(const transaction_t *tx, const master_keys_t *master_keys);

#endif /* TX_VALIDATE_H */
