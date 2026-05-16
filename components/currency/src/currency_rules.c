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

#include "sdkconfig.h"
#include "esp_log.h"

static const char *TAG = "currency_rules";

#if CONFIG_MESHPAY_TEST_SKIP_MINT_AUTHORITY
/*
 * Garde-fou Release : empeche toute production binaire avec le bypass
 * actif. Si quelqu'un oublie de desactiver l'option avant de passer
 * en mode RELEASE, le build echoue ici, pas en prod.
 */
#if CONFIG_SECURE_FLASH_ENCRYPTION_MODE_RELEASE
#error "MESHPAY_TEST_SKIP_MINT_AUTHORITY interdit en mode RELEASE — desactiver dans menuconfig."
#endif
static const char *TAG_TEST_AUTH = "currency_test";
#endif

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

    /*
     * [F-CU-011] Borne INCLUSIVE : la monnaie est valide jusqu'à et y
     * compris l'instant `valid_until`. La condition stricte `>` autorise
     * `current_time == valid_until`. Décision design 2026-05-16 :
     * conserver le statu quo (test `currency_validates_at_exact_expiry`).
     */
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

    /*
     * [F-CU-004] Garde explicite sur mint_authority_count == 0 :
     * sans autorité configurée, AUCUN MINT n'est autorisé. Sans cette
     * garde, la boucle for ne s'exécute pas et la fonction tombe
     * silencieusement dans NOT_AUTHORITY. On loggue explicitement la
     * cause pour aider au diagnostic en cas de corruption NVS.
     */
    if (config->mint_authority_count == 0) {
        ESP_LOGE(TAG, "currency_check_mint_authority: mint_authority_count=0 "
                      "(config NVS corrompue ?) — MINT refuse");
        return CURRENCY_ERR_NOT_AUTHORITY;
    }

#if CONFIG_MESHPAY_TEST_SKIP_MINT_AUTHORITY
    /*
     * TEST MODE : on accepte n'importe quelle cle signataire pour
     * permettre la propagation des MINT entre devices sur table.
     * Un WARN est emis a CHAQUE appel pour rester visible dans les
     * logs si l'option reste active par erreur.
     */
    ESP_LOGW(TAG_TEST_AUTH,
             "MINT AUTH BYPASSED — TEST MODE (currency_id=0x%08lx)",
             (unsigned long)config->currency_id);
    (void)signer_key;
    return CURRENCY_OK;
#else
    /* Chercher la clé dans la liste des autorités */
    for (uint8_t i = 0; i < config->mint_authority_count; i++) {
        if (public_key_equal(&config->mint_authorities[i], signer_key)) {
            return CURRENCY_OK;
        }
    }

    return CURRENCY_ERR_NOT_AUTHORITY;
#endif
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

    /*
     * Vérifier que le délai minimum est respecté.
     *
     * [F-CU-007] Le produit `last_transfer_time + transfer_cooldown_ms`
     * ne peut pas déborder uint64_t en pratique sur ESP32 (UINT64_MAX
     * en millisecondes ≈ 584 millions d'années). Non exploitable.
     */
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

    /*
     * [F-CU-008] `current_time == 0` désactive intentionnellement
     * cooldown et expiration (tests, premier sync horloge non encore
     * reçu). En production, ce cas représente la fenêtre de boot
     * avant première synchronisation maître et signale que les checks
     * temporels sont skippés. On loggue un warning pour rendre la
     * situation visible en debug ; le bypass est conservé par décision
     * design (2026-05-16).
     */
    if (current_time == 0) {
        ESP_LOGW(TAG, "currency_validate appele avec current_time=0 "
                      "(cooldown + expiry skippes) — boot ou test ?");
    }

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

/* ================================================================
 * Validation de la configuration elle-même
 * ================================================================ */

currency_err_t currency_config_validate(const currency_config_t *config)
{
    if (!config) return CURRENCY_ERR_NULL_PARAM;

    /*
     * [F-CU-006] Garde-fous anti-corruption NVS.
     *
     * Une config chargée depuis NVS sans validation peut provoquer :
     *  - effacement des soldes si `melt_bps > CURRENCY_BPS_SCALE`,
     *  - accès OOB sur mint_authorities[] si count > MAX,
     *  - boucle infinie de fonte si period == 0 avec enabled == true.
     *
     * On loggue chaque violation pour faciliter le diagnostic et on
     * retourne au premier échec (l'appelant doit basculer sur une
     * config par défaut).
     */
    if (config->mint_authority_count > CURRENCY_MAX_MINT_AUTHORITIES) {
        ESP_LOGE(TAG, "config invalide : mint_authority_count=%u > MAX=%u",
                 config->mint_authority_count,
                 CURRENCY_MAX_MINT_AUTHORITIES);
        return CURRENCY_ERR_NOT_AUTHORITY;
    }

    if (config->melt_bps > CURRENCY_BPS_SCALE) {
        ESP_LOGE(TAG, "config invalide : melt_bps=%u > SCALE=%u",
                 config->melt_bps, CURRENCY_BPS_SCALE);
        return CURRENCY_ERR_WRONG_ID;
    }

    if (config->melt_enabled && config->melt_period_seconds == 0) {
        ESP_LOGE(TAG, "config invalide : melt_enabled=true avec "
                      "melt_period_seconds=0");
        return CURRENCY_ERR_WRONG_ID;
    }

    if (config->max_transfer_amount > 0 &&
        config->min_transfer_amount > config->max_transfer_amount) {
        ESP_LOGE(TAG, "config invalide : min_transfer_amount=%lu > "
                      "max_transfer_amount=%lu",
                 (unsigned long)config->min_transfer_amount,
                 (unsigned long)config->max_transfer_amount);
        return CURRENCY_ERR_AMOUNT_TOO_HIGH;
    }

    return CURRENCY_OK;
}
