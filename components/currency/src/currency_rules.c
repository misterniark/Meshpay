/**
 * @file currency_rules.c
 * @brief Validation des transactions selon la configuration de la monnaie.
 *
 * Chaque fonction vérifie un aspect précis des règles monétaires.
 * La fonction currency_validate() les enchaîne dans l'ordre correct.
 *
 * Ordre de vérification :
 * 1. currency_id cohérent
 * 2. Monnaie non expirée
 * 3. Pour TRANSFER : cooldown, min/max amount, puis solde ≥ amount + fee
 * 4. Pour MINT : total_minted + amount ≤ max_supply
 */

#include "currency/currency_rules.h"
#include <string.h>

/* ================================================================
 * Vérifications individuelles
 * ================================================================ */

currency_err_t currency_check_id(const currency_config_t *config,
                                 const transaction_t *tx)
{
    if (!config || !tx) return CURRENCY_ERR_NULL_PARAM;

    if (tx->currency_id != config->currency_id) {
        return CURRENCY_ERR_WRONG_ID;
    }

    return CURRENCY_OK;
}

currency_err_t currency_check_expiry(const currency_config_t *config,
                                     uint64_t current_time)
{
    if (!config) return CURRENCY_ERR_NULL_PARAM;

    /* 0 = pas d'expiration */
    if (config->valid_until == 0) {
        return CURRENCY_OK;
    }

    if (current_time > config->valid_until) {
        return CURRENCY_ERR_EXPIRED;
    }

    return CURRENCY_OK;
}

currency_err_t currency_check_transfer_limits(const currency_config_t *config,
                                              const transaction_t *tx)
{
    if (!config || !tx) return CURRENCY_ERR_NULL_PARAM;

    /* Ne s'applique qu'aux TRANSFER */
    if (tx->type != TX_TYPE_TRANSFER) {
        return CURRENCY_OK;
    }

    /* Vérifier le montant minimum */
    if (config->min_transfer_amount > 0 && tx->amount < config->min_transfer_amount) {
        return CURRENCY_ERR_AMOUNT_TOO_LOW;
    }

    /* Vérifier le montant maximum (0 = pas de plafond) */
    if (config->max_transfer_amount > 0 && tx->amount > config->max_transfer_amount) {
        return CURRENCY_ERR_AMOUNT_TOO_HIGH;
    }

    return CURRENCY_OK;
}

currency_err_t currency_check_balance(const currency_config_t *config,
                                      const transaction_t *tx,
                                      uint32_t sender_balance)
{
    if (!config || !tx) return CURRENCY_ERR_NULL_PARAM;

    /* Ne s'applique qu'aux TRANSFER */
    if (tx->type != TX_TYPE_TRANSFER) {
        return CURRENCY_OK;
    }

    /*
     * Le coût total est amount + fee (stocké dans la transaction).
     * Les frais sont brûlés : l'émetteur paye, le destinataire
     * ne reçoit que le montant sans les frais.
     * Le fee est figé dans la TX au moment de sa création, ce qui
     * garantit la cohérence même si le taux de frais évolue.
     */
    uint64_t total_cost = (uint64_t)tx->amount + (uint64_t)tx->fee;
    if (total_cost > (uint64_t)sender_balance) {
        return CURRENCY_ERR_INSUFFICIENT;
    }

    return CURRENCY_OK;
}

currency_err_t currency_check_supply(const currency_config_t *config,
                                     const transaction_t *tx,
                                     uint64_t total_minted)
{
    if (!config || !tx) return CURRENCY_ERR_NULL_PARAM;

    /* Ne s'applique qu'aux MINT */
    if (tx->type != TX_TYPE_MINT) {
        return CURRENCY_OK;
    }

    /* 0 = pas de plafond */
    if (config->max_supply == 0) {
        return CURRENCY_OK;
    }

    /* Vérifier que le MINT ne dépasse pas le plafond */
    if (total_minted + (uint64_t)tx->amount > config->max_supply) {
        return CURRENCY_ERR_SUPPLY_EXCEEDED;
    }

    return CURRENCY_OK;
}

currency_err_t currency_check_mint_authority(const currency_config_t *config,
                                             const public_key_t *signer_key)
{
    if (!config || !signer_key) return CURRENCY_ERR_NULL_PARAM;

    /* Chercher la clé dans la liste des autorités */
    for (uint8_t i = 0; i < config->mint_authority_count; i++) {
        if (public_key_equal(&config->mint_authorities[i], signer_key)) {
            return CURRENCY_OK;
        }
    }

    return CURRENCY_ERR_NOT_AUTHORITY;
}

currency_err_t currency_check_cooldown(const currency_config_t *config,
                                       const transaction_t *tx,
                                       uint64_t last_transfer_time,
                                       uint64_t current_time)
{
    if (!config || !tx) return CURRENCY_ERR_NULL_PARAM;

    /* Ne s'applique qu'aux TRANSFER */
    if (tx->type != TX_TYPE_TRANSFER) {
        return CURRENCY_OK;
    }

    /* 0 = pas de cooldown */
    if (config->transfer_cooldown_ms == 0) {
        return CURRENCY_OK;
    }

    /* Pas de précédent TRANSFER → pas de cooldown à vérifier */
    if (last_transfer_time == 0) {
        return CURRENCY_OK;
    }

    /* Vérifier que le délai minimum est respecté */
    if (current_time < last_transfer_time + (uint64_t)config->transfer_cooldown_ms) {
        return CURRENCY_ERR_COOLDOWN;
    }

    return CURRENCY_OK;
}

/* ================================================================
 * Validation partielle (sans mint_authority)
 *
 * WARNING [H6] : Cette fonction ne vérifie PAS que le signataire
 * d'une transaction MINT est une autorité reconnue. L'appelant
 * DOIT appeler currency_check_mint_authority() séparément après
 * vérification de la signature cryptographique.
 * ================================================================ */

currency_err_t currency_validate(const currency_config_t *config,
                                 const transaction_t *tx,
                                 uint64_t current_time,
                                 uint32_t sender_balance,
                                 uint64_t total_minted,
                                 uint64_t last_transfer_time)
{
    if (!config || !tx) return CURRENCY_ERR_NULL_PARAM;

    currency_err_t err;

    /* 1. currency_id */
    err = currency_check_id(config, tx);
    if (err != CURRENCY_OK) return err;

    /* 2. Expiration (seulement si current_time > 0) */
    if (current_time > 0) {
        err = currency_check_expiry(config, current_time);
        if (err != CURRENCY_OK) return err;
    }

    /* 3. Règles spécifiques au type */
    if (tx->type == TX_TYPE_TRANSFER) {
        /* Cooldown (seulement si current_time > 0) */
        if (current_time > 0) {
            err = currency_check_cooldown(config, tx, last_transfer_time, current_time);
            if (err != CURRENCY_OK) return err;
        }

        /* Limites de montant */
        err = currency_check_transfer_limits(config, tx);
        if (err != CURRENCY_OK) return err;

        /* Solde suffisant (amount + fee) */
        err = currency_check_balance(config, tx, sender_balance);
        if (err != CURRENCY_OK) return err;
    }

    if (tx->type == TX_TYPE_MINT) {
        /* Plafond de supply */
        err = currency_check_supply(config, tx, total_minted);
        if (err != CURRENCY_OK) return err;
    }

    return CURRENCY_OK;
}
