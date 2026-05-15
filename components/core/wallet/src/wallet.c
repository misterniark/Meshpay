/**
 * @file wallet.c
 * @brief Implémentation du portefeuille — calcul de solde à partir du DAG.
 *
 * Le solde disponible est calculé en parcourant le DAG :
 *   solde = base_balance
 *         + somme des MINT/TRANSFER reçus (CONFIRMED)
 *         - somme des TRANSFER émis (CONFIRMED ou LOCKED)
 *
 * Les TX CANCELLED sont ignorées (le montant a été déverrouillé).
 */

#include "wallet/wallet.h"
#include "wallet/wallet_checkpoint.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>

/*
 * [F-DG-008] Helpers internes pour acquérir/relâcher le mutex récursif
 * du DAG depuis wallet.c. L'itération directe sur dag->transactions[]
 * dans wallet_get_balance et wallet_get_balance_for ne prenait aucun
 * mutex, créant une race condition avec dag_merge_transaction qui peut
 * être appelée depuis la tâche LoRa pendant que wallet_get_balance est
 * appelée depuis la tâche core. Le mutex DAG est récursif, donc le
 * prendre ici est sûr même si l'appelant le détient déjà
 * (dag_insert_and_track ne tient pas le mutex DAG pendant ses appels
 * à wallet_get_balance ; les autres flux ne le tiennent pas non plus).
 */
static inline void wallet_dag_lock(const dag_t *dag)
{
    if (dag != NULL && dag->mutex != NULL) {
        xSemaphoreTakeRecursive(dag->mutex, portMAX_DELAY);
    }
}

static inline void wallet_dag_unlock(const dag_t *dag)
{
    if (dag != NULL && dag->mutex != NULL) {
        xSemaphoreGiveRecursive(dag->mutex);
    }
}

esp_err_t wallet_init(wallet_t *wallet, const public_key_t *owner,
                      dag_t *dag, get_time_ms_fn get_time)
{
    if (wallet == NULL || owner == NULL || dag == NULL || get_time == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memcpy(&wallet->owner, owner, sizeof(public_key_t));
    wallet->dag = dag;
    wallet->get_time = get_time;
    wallet->last_melt_timestamp = 0;
    memset(&wallet->fee_recipient, 0, sizeof(public_key_t));

    return ESP_OK;
}

esp_err_t wallet_get_balance(const wallet_t *wallet, uint32_t base_balance,
                             uint32_t *available)
{
    if (wallet == NULL || available == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /*
     * Calcul du solde en parcourant toutes les transactions du DAG.
     * On utilise des int64 pour éviter les dépassements intermédiaires
     * (le solde final doit être >= 0 et tenir dans un uint32).
     *
     * [F-DG-008] On prend le mutex DAG pendant TOUT le scan pour que
     * dag->count et le contenu de dag->transactions[] soient lus de
     * manière atomique vis-à-vis d'un dag_merge_transaction concurrent.
     */
    wallet_dag_lock(wallet->dag);

    int64_t balance = (int64_t)base_balance;

    for (uint32_t i = 0; i < wallet->dag->count; i++) {
        const transaction_t *tx = &wallet->dag->transactions[i];

        /* Ignorer les transactions annulées */
        if (tx->status == TX_STATUS_CANCELLED) {
            continue;
        }

        /* Crédits reçus : MINT ou TRANSFER vers le propriétaire du wallet */
        if (public_key_equal(&tx->to, &wallet->owner)) {
            if (tx->status == TX_STATUS_CONFIRMED) {
                /*
                 * Le destinataire ne reçoit que le montant brut (amount).
                 * Les frais vont au fee_recipient (ou sont brûlés si non configuré).
                 */
                balance += tx->amount;
            }
            /*
             * Note : les TX LOCKED vers nous ne sont pas encore créditées.
             * Le crédit n'arrive qu'à la confirmation (CONFIRMED).
             */
        }

        /* Débits émis : TRANSFER depuis le propriétaire du wallet */
        if (tx->type == TX_TYPE_TRANSFER &&
            public_key_equal(&tx->from, &wallet->owner)) {
            /*
             * Les TX LOCKED et CONFIRMED sont comptées comme dépensées.
             * LOCKED = montant verrouillé, pas encore disponible.
             * CONFIRMED = montant définitivement dépensé.
             *
             * Le coût total pour l'émetteur = amount + fee.
             * Le fee est stocké dans la transaction au moment de sa création,
             * garantissant la cohérence même si le taux de frais change.
             */
            if (tx->status == TX_STATUS_LOCKED || tx->status == TX_STATUS_CONFIRMED) {
                balance -= (int64_t)tx->amount + (int64_t)tx->fee;
            }
        }

        /*
         * Crédit des frais au fee_recipient (si configuré).
         *
         * Si fee_recipient est non-zéro ET que le propriétaire du wallet
         * EST le fee_recipient, on crédite les fees de toutes les TX
         * TRANSFER confirmées. Les fees ne sont crédités qu'à la
         * confirmation (pas LOCKED) car le transfert n'est pas encore
         * finalisé.
         *
         * Si fee_recipient est tout-zéro, les fees sont brûlés (comportement
         * historique : ils disparaissent de la masse monétaire).
         */
        if (tx->type == TX_TYPE_TRANSFER && tx->fee > 0 &&
            tx->status == TX_STATUS_CONFIRMED &&
            !public_key_is_zero(&wallet->fee_recipient) &&
            public_key_equal(&wallet->fee_recipient, &wallet->owner)) {
            balance += (int64_t)tx->fee;
        }
    }

    /* Protection contre les soldes négatifs (ne devrait pas arriver) */
    if (balance < 0) {
        balance = 0;
    }

    *available = (uint32_t)balance;
    wallet_dag_unlock(wallet->dag);
    return ESP_OK;
}

esp_err_t wallet_get_total_minted(const dag_t *dag, uint64_t *total_minted)
{
    if (dag == NULL || total_minted == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /* [F-DG-008] Scan sous mutex pour cohérence avec dag_merge concurrent. */
    wallet_dag_lock(dag);

    uint64_t total = 0;

    for (uint32_t i = 0; i < dag->count; i++) {
        const transaction_t *tx = &dag->transactions[i];

        /*
         * Sommer les MINT confirmés uniquement.
         * [F-DG-025] Garde-fou de saturation : si l'addition dépasse
         * UINT64_MAX (cas théorique extrême), on sature à UINT64_MAX
         * plutôt que de produire un wrap-around silencieux. Cohérent
         * avec le guard [H1] de wallet_checkpoint.c.
         */
        if (tx->type == TX_TYPE_MINT && tx->status == TX_STATUS_CONFIRMED) {
            if (total > UINT64_MAX - tx->amount) {
                total = UINT64_MAX;
            } else {
                total += tx->amount;
            }
        }
    }

    *total_minted = total;
    wallet_dag_unlock(dag);
    return ESP_OK;
}

esp_err_t wallet_get_balance_for(const dag_t *dag,
                                 const checkpoint_t *checkpoint,
                                 const public_key_t *target,
                                 const public_key_t *fee_recipient,
                                 uint32_t *balance)
{
    if (dag == NULL || target == NULL || balance == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /*
     * 1. Base = solde du target dans le checkpoint (0 si absent ou NULL).
     *    Depuis dag_prune_before, les TX consolidees ne sont plus dans
     *    le DAG, donc on doit les recuperer depuis le checkpoint.
     */
    int64_t acc = 0;
    if (checkpoint != NULL) {
        uint32_t base = 0;
        (void)checkpoint_get_balance(checkpoint, target, &base);
        acc = (int64_t)base;
    }

    /*
     * 2. Parcourir le DAG (TX post-checkpoint) et appliquer les
     *    credits/debits pour le target.
     *    Meme logique que wallet_get_balance mais la pubkey cible et
     *    le fee_recipient sont des parametres (pas tires du wallet).
     *
     * [F-DG-008] Scan sous mutex DAG pour cohérence vs dag_merge.
     */
    bool has_fee_recipient = (fee_recipient != NULL &&
                              !public_key_is_zero(fee_recipient));

    wallet_dag_lock(dag);

    for (uint32_t i = 0; i < dag->count; i++) {
        const transaction_t *tx = &dag->transactions[i];

        if (tx->status == TX_STATUS_CANCELLED) {
            continue;
        }

        /* Credits recus (MINT + TRANSFER vers target, CONFIRMED uniquement) */
        if (public_key_equal(&tx->to, target) &&
            tx->status == TX_STATUS_CONFIRMED) {
            acc += tx->amount;
        }

        /* Debits emis (TRANSFER depuis target, LOCKED ou CONFIRMED) */
        if (tx->type == TX_TYPE_TRANSFER &&
            public_key_equal(&tx->from, target) &&
            (tx->status == TX_STATUS_LOCKED ||
             tx->status == TX_STATUS_CONFIRMED)) {
            acc -= (int64_t)tx->amount + (int64_t)tx->fee;
        }

        /* Credit des fees si target == fee_recipient (CONFIRMED uniquement) */
        if (has_fee_recipient &&
            tx->type == TX_TYPE_TRANSFER && tx->fee > 0 &&
            tx->status == TX_STATUS_CONFIRMED &&
            public_key_equal(fee_recipient, target)) {
            acc += (int64_t)tx->fee;
        }
    }

    /* Saturer a [0, UINT32_MAX] pour se proteger des cas limites */
    if (acc < 0) acc = 0;
    if (acc > UINT32_MAX) acc = UINT32_MAX;

    *balance = (uint32_t)acc;
    wallet_dag_unlock(dag);
    return ESP_OK;
}
