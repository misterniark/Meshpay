/**
 * @file currency_rules.h
 * @brief Validation des transactions selon la configuration de la monnaie.
 *
 * Ce module vérifie qu'une transaction respecte les règles métier
 * définies dans la configuration de la monnaie :
 * - Le currency_id correspond à la config locale
 * - La monnaie n'a pas expiré (valid_until)
 * - Les montants respectent min/max transfer
 * - Le solde couvre amount + tx->fee (fee stocké dans la transaction)
 * - Les MINT ne dépassent pas le max_supply
 * - Les MINT sont signés par une autorité reconnue
 *
 * Ces vérifications s'intercalent entre tx_validate_structure()
 * et tx_validate_signature() dans le pipeline de validation.
 */

#ifndef CURRENCY_RULES_H
#define CURRENCY_RULES_H

#include "currency/currency_config.h"
#include "transaction/tx_types.h"
#include <stdint.h>

/* ================================================================
 * Codes d'erreur
 * ================================================================ */

/** Codes d'erreur spécifiques à la validation currency */
typedef enum {
    CURRENCY_OK                  =  0,
    CURRENCY_ERR_NULL_PARAM      = -1,
    CURRENCY_ERR_WRONG_ID        = -2, /* currency_id ne correspond pas */
    CURRENCY_ERR_EXPIRED         = -3, /* Monnaie expirée (valid_until dépassé) */
    CURRENCY_ERR_AMOUNT_TOO_LOW  = -4, /* Montant < min_transfer_amount */
    CURRENCY_ERR_AMOUNT_TOO_HIGH = -5, /* Montant > max_transfer_amount */
    CURRENCY_ERR_SUPPLY_EXCEEDED = -6, /* MINT dépasserait max_supply */
    CURRENCY_ERR_NOT_AUTHORITY   = -7, /* Signataire MINT non autorisé */
    CURRENCY_ERR_INSUFFICIENT    = -8, /* Solde insuffisant (amount + fee) */
    CURRENCY_ERR_COOLDOWN        = -9, /* Cooldown entre TRANSFER non respecté */
} currency_err_t;

/* ================================================================
 * API publique
 * ================================================================ */

/**
 * Vérifier que le currency_id de la transaction correspond à la config.
 *
 * Première vérification du pipeline : si le currency_id ne correspond pas,
 * la TX est rejetée immédiatement (elle appartient à un autre réseau).
 *
 * @param config Configuration de la monnaie locale
 * @param tx     Transaction à vérifier
 * @return CURRENCY_OK ou CURRENCY_ERR_WRONG_ID
 */
currency_err_t currency_check_id(const currency_config_t *config,
                                 const transaction_t *tx);

/**
 * Vérifier que la monnaie n'a pas expiré.
 *
 * Compare current_time à config->valid_until. Si valid_until == 0,
 * la monnaie n'expire jamais.
 *
 * @param config       Configuration de la monnaie
 * @param current_time Temps courant en millisecondes
 * @return CURRENCY_OK ou CURRENCY_ERR_EXPIRED
 */
currency_err_t currency_check_expiry(const currency_config_t *config,
                                     uint64_t current_time);

/**
 * Vérifier les limites de montant d'un TRANSFER.
 *
 * Contrôle que amount respecte min_transfer_amount et max_transfer_amount.
 * Ne s'applique qu'aux TRANSFER (pas aux MINT).
 *
 * @param config Configuration de la monnaie
 * @param tx     Transaction à vérifier
 * @return CURRENCY_OK, CURRENCY_ERR_AMOUNT_TOO_LOW ou CURRENCY_ERR_AMOUNT_TOO_HIGH
 */
currency_err_t currency_check_transfer_limits(const currency_config_t *config,
                                              const transaction_t *tx);

/**
 * Vérifier que le solde de l'émetteur couvre amount + tx->fee.
 *
 * Ne s'applique qu'aux TRANSFER. Le fee est brûlé (déduit du solde
 * de l'émetteur, non crédité au destinataire). Le fee est stocké
 * directement dans la transaction au moment de sa création.
 *
 * @param config            Configuration de la monnaie
 * @param tx                Transaction à vérifier
 * @param sender_balance    Solde disponible de l'émetteur
 * @return CURRENCY_OK ou CURRENCY_ERR_INSUFFICIENT
 */
currency_err_t currency_check_balance(const currency_config_t *config,
                                      const transaction_t *tx,
                                      uint32_t sender_balance);

/**
 * Vérifier qu'un MINT ne dépasse pas le max_supply.
 *
 * Si max_supply == 0, pas de plafond. Sinon, vérifie que
 * total_minted + tx->amount <= max_supply.
 *
 * @param config       Configuration de la monnaie
 * @param tx           Transaction MINT à vérifier
 * @param total_minted Total déjà créé pour cette monnaie
 * @return CURRENCY_OK ou CURRENCY_ERR_SUPPLY_EXCEEDED
 */
currency_err_t currency_check_supply(const currency_config_t *config,
                                     const transaction_t *tx,
                                     uint64_t total_minted);

/**
 * Vérifier qu'un MINT est signé par une autorité reconnue.
 *
 * Cherche la clé du signataire dans config->mint_authorities.
 * Pour les TRANSFER, retourne toujours CURRENCY_OK.
 *
 * @param config     Configuration de la monnaie
 * @param signer_key Clé publique du signataire de la TX
 * @return CURRENCY_OK ou CURRENCY_ERR_NOT_AUTHORITY
 */
currency_err_t currency_check_mint_authority(const currency_config_t *config,
                                             const public_key_t *signer_key);

/**
 * Vérifier le cooldown entre deux TRANSFER émis par le même device.
 *
 * Compare le timestamp de la dernière TX émise avec le temps courant.
 * Si l'écart est inférieur à transfer_cooldown_ms, le TRANSFER est rejeté.
 * Ne s'applique qu'aux TRANSFER. Si cooldown == 0, toujours ok.
 *
 * @param config             Configuration de la monnaie
 * @param tx                 Transaction à vérifier
 * @param last_transfer_time Timestamp de la dernière TX émise par ce device (ms)
 * @param current_time       Temps courant (ms)
 * @return CURRENCY_OK ou CURRENCY_ERR_COOLDOWN
 */
currency_err_t currency_check_cooldown(const currency_config_t *config,
                                       const transaction_t *tx,
                                       uint64_t last_transfer_time,
                                       uint64_t current_time);

/**
 * Validation PARTIELLE d'une transaction selon les règles currency.
 *
 * @warning [H6] Cette fonction ne vérifie PAS l'autorité MINT
 *          (mint_authority). Pour les transactions MINT, l'appelant
 *          DOIT appeler currency_check_mint_authority() séparément
 *          après vérification de la signature. Le nom "validate"
 *          peut induire en erreur : cette validation est incomplète
 *          pour les MINT.
 *
 * Exécute les vérifications suivantes dans l'ordre :
 * 1. currency_id
 * 2. expiry (si current_time > 0)
 * 3. Pour TRANSFER : cooldown, limites montant, puis solde (amount + fee)
 * 4. Pour MINT : supply (mais PAS mint_authority)
 *
 * @param config              Configuration de la monnaie
 * @param tx                  Transaction à vérifier
 * @param current_time        Temps courant (0 = ne pas vérifier l'expiration ni le cooldown)
 * @param sender_balance      Solde de l'émetteur (ignoré pour MINT)
 * @param total_minted        Total déjà créé (ignoré pour TRANSFER)
 * @param last_transfer_time  Timestamp du dernier TRANSFER émis (0 = pas de précédent)
 * @return CURRENCY_OK ou le premier code d'erreur rencontré
 */
currency_err_t currency_validate(const currency_config_t *config,
                                 const transaction_t *tx,
                                 uint64_t current_time,
                                 uint32_t sender_balance,
                                 uint64_t total_minted,
                                 uint64_t last_transfer_time);

/**
 * Vérifier l'intégrité d'une `currency_config_t` chargée depuis NVS.
 *
 * [F-CU-006] À appeler systématiquement après tout chargement de la
 * config depuis le stockage persistant. Une config corrompue peut
 * provoquer :
 *  - effacement silencieux des soldes (`melt_bps > 10000` → fonte > 100%)
 *  - accès OOB sur `mint_authorities[]` si `mint_authority_count` excède
 *    `CURRENCY_MAX_MINT_AUTHORITIES`
 *  - boucle infinie de ticks si `melt_period_seconds == 0` avec
 *    `melt_enabled == true`
 *
 * Les invariants vérifiés :
 *   - `mint_authority_count <= CURRENCY_MAX_MINT_AUTHORITIES`
 *   - `melt_bps <= 10000` (CURRENCY_BPS_SCALE)
 *   - Si `melt_enabled == true`, alors `melt_period_seconds > 0`
 *   - `min_transfer_amount <= max_transfer_amount` (sauf si max == 0)
 *
 * @param config Configuration à valider
 * @return CURRENCY_OK ou CURRENCY_ERR_* si invariant violé
 */
currency_err_t currency_config_validate(const currency_config_t *config);

#endif /* CURRENCY_RULES_H */
