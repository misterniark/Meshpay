/**
 * @file currency_config_init.c
 * @brief Implementation de l'init currency (voir currency_config_init.h).
 */

#include "currency_config_init.h"

#include <string.h>

#include "esp_log.h"

#include "app_state.h"
#include "currency/currency_config.h"
#include "currency/currency_rules.h"

static const char *TAG = "currency_init";

void init_currency_config(void)
{
    memset(&s_currency, 0, sizeof(currency_config_t));

    s_currency.currency_id = 0x00000001;
    strncpy(s_currency.name, "TestCoin", CURRENCY_NAME_MAX);
    strncpy(s_currency.symbol, "TST", CURRENCY_SYMBOL_MAX);
    strncpy(s_currency.description, "Monnaie de test", CURRENCY_DESCRIPTION_MAX);
    s_currency.decimals = 2;

    /* Pas de plafond de supply, pas d'expiration. */
    s_currency.max_supply  = 0;
    s_currency.valid_until = 0;

    /* Solde initial au premier boot. */
    s_currency.initial_balance = 1000;

    /* Regles de transfert. */
    s_currency.min_transfer_amount  = 1;
    s_currency.max_transfer_amount  = 0;  /* pas de plafond */
    s_currency.transfer_fee         = 0;  /* pas de frais */
    s_currency.transfer_cooldown_ms = 0;  /* pas de cooldown */

    /*
     * Fonte.
     *
     * [F-MN-013] Cohérence avec F-CU-002 : la fonte automatique a été
     * suspendue le 2026-05-16 en attendant la conception d'un mécanisme
     * robuste (quorum d'application synchronisé, persistance NVS,
     * convergence garantie en présence de devices intermittents).
     *
     * On désactive `melt_enabled` à la source pour que `core_task` ne
     * tente plus d'appliquer `apply_pending_melt` au solde local (qui
     * créait une incohérence entre l'affichage et le checkpoint NVS).
     *
     * Les paramètres ci-dessous sont conservés en l'état pour faciliter
     * une réactivation propre dans un Lot dédié. Bouton facile :
     * passer `melt_enabled = true` quand le mécanisme est prêt.
     */
    s_currency.melt_enabled         = false;
    s_currency.melt_period_seconds  = 86400;       /* 1 tick = 1 jour */
    s_currency.melt_volume_mode     = MELT_MODE_BPS;
    s_currency.melt_bps             = 100;          /* 1% par jour */
    s_currency.melt_fixed_amount    = 0;

    /* Autorite MINT : notre propre cle (au premier boot). */
    memcpy(&s_currency.mint_authorities[0], &s_keypair.public_key,
           sizeof(public_key_t));
    s_currency.mint_authority_count = 1;

    /*
     * [F-CU-006] Filet de sécurité : valider la config produite avant
     * de la laisser sortir. Aujourd'hui la config est hardcodée, donc
     * une violation signale un bug dans ce fichier. Demain (chargement
     * NVS), cette validation interceptera une config corrompue avant
     * qu'elle n'effectue de dégâts (effacement des soldes, accès OOB).
     */
    currency_err_t verr = currency_config_validate(&s_currency);
    if (verr != CURRENCY_OK) {
        ESP_LOGE(TAG, "Config currency hardcodee invalide (err=%d) — "
                      "bug dans init_currency_config", verr);
    }
}
