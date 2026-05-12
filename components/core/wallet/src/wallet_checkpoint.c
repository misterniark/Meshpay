/**
 * @file wallet_checkpoint.c
 * @brief Implémentation des checkpoints (snapshots de soldes).
 *
 * Un checkpoint est créé en parcourant le DAG et en accumulant
 * les crédits/débits de chaque compte. Le checkpoint précédent
 * sert de base pour les soldes initiaux.
 */

#include "wallet/wallet_checkpoint.h"
#include <string.h>
#include <stdint.h>
#include "esp_log.h"

/** Tag pour les logs de ce module */
static const char *TAG = "wallet_checkpoint";

/**
 * @brief Cherche ou crée une entrée pour une clé publique dans le checkpoint.
 *
 * Si la clé existe déjà, retourne son index.
 * Sinon, crée une nouvelle entrée avec un solde initial de 0.
 *
 * @param checkpoint Checkpoint en cours de construction
 * @param key        Clé publique à chercher/créer
 * @return Index de l'entrée, ou -1 si le tableau est plein
 */
static int find_or_create_account(checkpoint_t *checkpoint, const public_key_t *key)
{
    /* Chercher une entrée existante */
    for (uint32_t i = 0; i < checkpoint->account_count; i++) {
        if (public_key_equal(&checkpoint->accounts[i].key, key)) {
            return (int)i;
        }
    }

    /* Créer une nouvelle entrée */
    if (checkpoint->account_count >= CHECKPOINT_MAX_ACCOUNTS) {
        return -1;  /* Tableau plein */
    }

    int idx = (int)checkpoint->account_count;
    memcpy(&checkpoint->accounts[idx].key, key, sizeof(public_key_t));
    checkpoint->accounts[idx].balance = 0;
    checkpoint->account_count++;

    return idx;
}

esp_err_t checkpoint_create(const dag_t *dag, const checkpoint_t *base,
                            const public_key_t *fee_recipient,
                            checkpoint_t *checkpoint)
{
    if (dag == NULL || checkpoint == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(checkpoint, 0, sizeof(checkpoint_t));

    /*
     * Si un checkpoint précédent existe, copier ses soldes comme base.
     * Les transactions du DAG actuel seront appliquées par-dessus.
     */
    if (base != NULL) {
        /*
         * Valider account_count avant le memcpy pour éviter un débordement
         * de buffer. Un checkpoint corrompu (chargé depuis la flash ou reçu
         * du réseau) pourrait avoir un account_count supérieur à la taille
         * du tableau, provoquant une écriture hors limites.
         */
        if (base->account_count > CHECKPOINT_MAX_ACCOUNTS) {
            ESP_LOGE(TAG, "account_count du checkpoint base invalide : %lu > %d",
                     (unsigned long)base->account_count, CHECKPOINT_MAX_ACCOUNTS);
            return ESP_ERR_INVALID_ARG;
        }
        memcpy(checkpoint->accounts, base->accounts,
               base->account_count * sizeof(checkpoint_entry_t));
        checkpoint->account_count = base->account_count;
    }

    /*
     * Parcourir toutes les transactions confirmées du DAG
     * et mettre à jour les soldes.
     */
    uint64_t latest_timestamp = 0;
    hash_t latest_tx_id;
    memset(&latest_tx_id, 0, sizeof(hash_t));

    for (uint32_t i = 0; i < dag->count; i++) {
        const transaction_t *tx = &dag->transactions[i];

        /* Ne compter que les transactions confirmées */
        if (tx->status != TX_STATUS_CONFIRMED) {
            continue;
        }

        /* Créditer le destinataire (reçoit uniquement amount, pas les frais) */
        int to_idx = find_or_create_account(checkpoint, &tx->to);
        if (to_idx < 0) {
            return ESP_ERR_NO_MEM;
        }

        /*
         * [H1] Vérification d'overflow avant l'addition du solde.
         * Si le solde actuel + le montant dépasse UINT32_MAX, on refuse
         * l'opération pour éviter un dépassement silencieux.
         */
        if (checkpoint->accounts[to_idx].balance > UINT32_MAX - tx->amount) {
            ESP_LOGW(TAG, "Overflow de solde détecté pour le destinataire");
            return ESP_ERR_INVALID_STATE;
        }
        checkpoint->accounts[to_idx].balance += tx->amount;

        /* Débiter l'émetteur (sauf pour les MINT, qui n'ont pas d'émetteur) */
        if (tx->type == TX_TYPE_TRANSFER) {
            int from_idx = find_or_create_account(checkpoint, &tx->from);
            if (from_idx < 0) {
                return ESP_ERR_NO_MEM;
            }
            /*
             * Le coût total pour l'émetteur = amount + fee.
             * Le fee est stocké dans la transaction au moment de sa création,
             * garantissant la cohérence même si le taux de frais change.
             */

            /*
             * [H1] Vérification d'overflow sur le calcul du coût total.
             * Si amount + fee dépasse UINT32_MAX, on refuse l'opération.
             */
            if (tx->amount > UINT32_MAX - tx->fee) {
                ESP_LOGW(TAG, "Overflow de total_cost détecté (amount + fee)");
                return ESP_ERR_INVALID_STATE;
            }
            uint32_t total_cost = tx->amount + tx->fee;
            /* Protection contre le dépassement négatif */
            if (checkpoint->accounts[from_idx].balance >= total_cost) {
                checkpoint->accounts[from_idx].balance -= total_cost;
            } else {
                checkpoint->accounts[from_idx].balance = 0;
            }

            /*
             * Créditer les frais au fee_recipient s'il est configuré.
             * Si fee_recipient est NULL ou tout-zéro, les fees sont brûlés
             * (retirés de la masse monétaire — comportement historique).
             * Sinon, le fee est crédité au fee_recipient (commission pour
             * l'organisateur du réseau).
             */
            if (tx->fee > 0 && fee_recipient != NULL &&
                !public_key_is_zero(fee_recipient)) {
                int fee_idx = find_or_create_account(checkpoint, fee_recipient);
                if (fee_idx < 0) {
                    return ESP_ERR_NO_MEM;
                }
                /* [H1] Vérification d'overflow avant crédit du fee */
                if (checkpoint->accounts[fee_idx].balance > UINT32_MAX - tx->fee) {
                    ESP_LOGW(TAG, "Overflow de solde détecté pour le fee_recipient");
                    return ESP_ERR_INVALID_STATE;
                }
                checkpoint->accounts[fee_idx].balance += tx->fee;
            }
        }

        /* Garder trace de la dernière TX pour le champ last_tx_id */
        if (tx->timestamp >= latest_timestamp) {
            latest_timestamp = tx->timestamp;
            memcpy(&latest_tx_id, &tx->id, sizeof(hash_t));
        }
    }

    checkpoint->timestamp = latest_timestamp;
    memcpy(&checkpoint->last_tx_id, &latest_tx_id, sizeof(hash_t));

    return ESP_OK;
}

esp_err_t checkpoint_get_balance(const checkpoint_t *checkpoint,
                                 const public_key_t *key,
                                 uint32_t *balance)
{
    if (checkpoint == NULL || key == NULL || balance == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    for (uint32_t i = 0; i < checkpoint->account_count; i++) {
        if (public_key_equal(&checkpoint->accounts[i].key, key)) {
            *balance = checkpoint->accounts[i].balance;
            return ESP_OK;
        }
    }

    /* Compte non trouvé — solde implicite de 0 */
    *balance = 0;
    return ESP_ERR_NOT_FOUND;
}
