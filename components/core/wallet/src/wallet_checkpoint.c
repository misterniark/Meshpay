/**
 * @file wallet_checkpoint.c
 * @brief Implémentation des checkpoints (snapshots de soldes).
 *
 * Un checkpoint est créé en parcourant le DAG et en accumulant
 * les crédits/débits de chaque compte. Le checkpoint précédent
 * sert de base pour les soldes initiaux.
 */

#include "wallet/wallet_checkpoint.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>
#include <stdint.h>
#include "esp_log.h"

/** Tag pour les logs de ce module */
static const char *TAG = "wallet_checkpoint";

/*
 * [F-WL-001] Helpers identiques à ceux de wallet.c (récursif → safe).
 */
static inline void chk_dag_lock(const dag_t *dag)
{
    if (dag != NULL && dag->mutex != NULL) {
        xSemaphoreTakeRecursive(dag->mutex, portMAX_DELAY);
    }
}

static inline void chk_dag_unlock(const dag_t *dag)
{
    if (dag != NULL && dag->mutex != NULL) {
        xSemaphoreGiveRecursive(dag->mutex);
    }
}

/*
 * [F-WL-006] Compare deux entrées par leur clé publique (ordre
 * lexicographique des octets). Utilisé par le tri final déterministe.
 */
static int compare_entries(const checkpoint_entry_t *a,
                           const checkpoint_entry_t *b)
{
    return memcmp(a->key.bytes, b->key.bytes, CRYPTO_PUBLIC_KEY_SIZE);
}

/*
 * Insertion-sort sur `accounts[]`. Tableau borné à 250 entrées, donc
 * O(n²) acceptable (max ~62 500 comparaisons à chaque checkpoint, soit
 * quelques millisecondes sur ESP32-S3).
 */
static void sort_accounts(checkpoint_t *checkpoint)
{
    for (uint32_t i = 1; i < checkpoint->account_count; i++) {
        checkpoint_entry_t key = checkpoint->accounts[i];
        int32_t j = (int32_t)i - 1;
        while (j >= 0 &&
               compare_entries(&checkpoint->accounts[j], &key) > 0) {
            checkpoint->accounts[j + 1] = checkpoint->accounts[j];
            j--;
        }
        checkpoint->accounts[j + 1] = key;
    }
}

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
    return checkpoint_create_ext(dag, base, fee_recipient, checkpoint, NULL);
}

esp_err_t checkpoint_create_ext(const dag_t *dag, const checkpoint_t *base,
                                const public_key_t *fee_recipient,
                                checkpoint_t *checkpoint,
                                hash_t *out_failed_tx_id)
{
    if (dag == NULL || checkpoint == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(checkpoint, 0, sizeof(checkpoint_t));
    if (out_failed_tx_id != NULL) {
        memset(out_failed_tx_id, 0, sizeof(hash_t));
    }

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

        /*
         * [F-WL-007] Propager `last_melt_timestamp` depuis le base.
         * Sans cette recopie, le mécanisme de fonte (quand réactivé)
         * repart de t=0 à chaque checkpoint et applique des ticks
         * de rattrapage injustifiés.
         */
        checkpoint->last_melt_timestamp = base->last_melt_timestamp;
    }

    /*
     * [F-WL-001] Acquérir le mutex DAG pendant tout le parcours pour
     * garantir l'atomicité vs un dag_merge_transaction concurrent. Le
     * mutex étant récursif, c'est sûr même si l'appelant le détient
     * déjà.
     */
    chk_dag_lock(dag);

    /*
     * Parcourir toutes les transactions confirmées du DAG
     * et mettre à jour les soldes.
     */
    uint64_t latest_timestamp = 0;
    hash_t latest_tx_id;
    memset(&latest_tx_id, 0, sizeof(hash_t));

    esp_err_t result = ESP_OK;

    for (uint32_t i = 0; i < dag->count; i++) {
        const transaction_t *tx = &dag->transactions[i];

        /* Ne compter que les transactions confirmées */
        if (tx->status != TX_STATUS_CONFIRMED) {
            continue;
        }

        /* Créditer le destinataire (reçoit uniquement amount, pas les frais) */
        int to_idx = find_or_create_account(checkpoint, &tx->to);
        if (to_idx < 0) {
            ESP_LOGW(TAG, "Saturation CHECKPOINT_MAX_ACCOUNTS au destinataire "
                          "(TX index=%lu, type=%d)", (unsigned long)i, tx->type);
            if (out_failed_tx_id != NULL) {
                memcpy(out_failed_tx_id, &tx->id, sizeof(hash_t));
            }
            result = ESP_ERR_NO_MEM;
            goto cleanup;
        }

        /*
         * [H1] Vérification d'overflow avant l'addition du solde.
         * Si le solde actuel + le montant dépasse UINT32_MAX, on refuse
         * l'opération pour éviter un dépassement silencieux.
         * [F-WL-004] L'ID de la TX fautive est remonté pour permettre
         * à l'appelant de la marquer CANCELLED.
         */
        if (checkpoint->accounts[to_idx].balance > UINT32_MAX - tx->amount) {
            ESP_LOGW(TAG, "Overflow de solde détecté pour le destinataire "
                          "(TX index=%lu, amount=%lu)",
                     (unsigned long)i, (unsigned long)tx->amount);
            if (out_failed_tx_id != NULL) {
                memcpy(out_failed_tx_id, &tx->id, sizeof(hash_t));
            }
            result = ESP_ERR_INVALID_STATE;
            goto cleanup;
        }
        checkpoint->accounts[to_idx].balance += tx->amount;

        /* Débiter l'émetteur (sauf pour les MINT, qui n'ont pas d'émetteur) */
        if (tx->type == TX_TYPE_TRANSFER) {
            int from_idx = find_or_create_account(checkpoint, &tx->from);
            if (from_idx < 0) {
                ESP_LOGW(TAG, "Saturation CHECKPOINT_MAX_ACCOUNTS a l'emetteur "
                              "(TX index=%lu)", (unsigned long)i);
                if (out_failed_tx_id != NULL) {
                    memcpy(out_failed_tx_id, &tx->id, sizeof(hash_t));
                }
                result = ESP_ERR_NO_MEM;
                goto cleanup;
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
                ESP_LOGW(TAG, "Overflow de total_cost détecté "
                              "(TX index=%lu, amount=%lu, fee=%lu)",
                         (unsigned long)i, (unsigned long)tx->amount,
                         (unsigned long)tx->fee);
                if (out_failed_tx_id != NULL) {
                    memcpy(out_failed_tx_id, &tx->id, sizeof(hash_t));
                }
                result = ESP_ERR_INVALID_STATE;
                goto cleanup;
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
                    ESP_LOGW(TAG, "Saturation CHECKPOINT_MAX_ACCOUNTS au "
                                  "fee_recipient (TX index=%lu)",
                             (unsigned long)i);
                    if (out_failed_tx_id != NULL) {
                        memcpy(out_failed_tx_id, &tx->id, sizeof(hash_t));
                    }
                    result = ESP_ERR_NO_MEM;
                    goto cleanup;
                }
                /* [H1] Vérification d'overflow avant crédit du fee */
                if (checkpoint->accounts[fee_idx].balance > UINT32_MAX - tx->fee) {
                    ESP_LOGW(TAG, "Overflow de solde détecté pour le "
                                  "fee_recipient (TX index=%lu, fee=%lu)",
                             (unsigned long)i, (unsigned long)tx->fee);
                    if (out_failed_tx_id != NULL) {
                        memcpy(out_failed_tx_id, &tx->id, sizeof(hash_t));
                    }
                    result = ESP_ERR_INVALID_STATE;
                    goto cleanup;
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

    /*
     * [F-WL-006] Tri lexicographique final des `accounts[]` pour
     * produire un checkpoint structurellement déterministe entre
     * devices (préparation consensus inter-pairs).
     */
    sort_accounts(checkpoint);

cleanup:
    chk_dag_unlock(dag);
    return result;
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
