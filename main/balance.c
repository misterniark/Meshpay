/**
 * @file balance.c
 * @brief Implementation du calcul de solde (voir balance.h).
 */

#include "balance.h"

#include "app_state.h"
#include "wallet/wallet.h"
#include "wallet/wallet_checkpoint.h"

esp_err_t compute_owner_balance(uint32_t *out_balance)
{
    if (out_balance == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /*
     * Base = solde consolide dans le checkpoint.
     * checkpoint_get_balance retourne ESP_ERR_NOT_FOUND si owner absent
     * → base reste a 0 (pas une erreur).
     */
    uint32_t base = 0;
    (void)checkpoint_get_balance(&s_checkpoint, &s_keypair.public_key, &base);

    /*
     * Appliquer les TX du DAG post-checkpoint. dag_prune_before garantit
     * que le DAG ne contient que les TX > checkpoint.timestamp.
     */
    return wallet_get_balance(&s_wallet, base, out_balance);
}

uint32_t ui_get_owner_balance(void)
{
    uint32_t balance = 0;
    compute_owner_balance(&balance);
    return balance;
}
