/**
 * @file wallet.h
 * @brief Gestion du portefeuille (solde et calcul à partir du DAG).
 *
 * Le wallet calcule le solde d'un device en parcourant le DAG.
 * Il distingue le solde disponible (utilisable pour payer) du solde
 * verrouillé (en attente de confirmation).
 *
 * Le wallet ne stocke pas directement dans le NVS ou la Flash —
 * il utilise des interfaces injectables pour la persistance (voir
 * wallet_checkpoint.h).
 *
 * L'horodatage est fourni via une fonction injectable pour permettre
 * les tests unitaires sans dépendance sur esp_timer.
 */

#ifndef WALLET_H
#define WALLET_H

#include "dag/dag.h"
#include "crypto/crypto_types.h"
#include "wallet/wallet_checkpoint.h"
#include "esp_err.h"
#include <stdint.h>

/**
 * @brief Fonction de récupération du temps courant (injectable).
 *
 * En production : wrapper autour de esp_timer_get_time() / RTC.
 * En test : mock retournant une valeur contrôlée.
 *
 * @return Timestamp courant en millisecondes
 */
typedef uint64_t (*get_time_ms_fn)(void);

/**
 * @brief Structure du portefeuille.
 *
 * Contient l'identité du device (clé publique), une référence au DAG,
 * et la fonction de temps injectable.
 */
typedef struct {
    public_key_t owner;      /**< Clé publique du propriétaire du wallet */
    dag_t       *dag;        /**< Référence au DAG (non possédé, géré ailleurs) */
    get_time_ms_fn get_time; /**< Fonction de récupération du temps courant */
    uint64_t last_melt_timestamp; /**< Timestamp du dernier traitement de fonte (ms). 0 = jamais appliqué. */
    public_key_t fee_recipient;   /**< Clé du destinataire des frais. Tout-zéro = fees brûlés. */
} wallet_t;

/**
 * @brief Initialise un wallet.
 *
 * @param[out] wallet   Wallet à initialiser
 * @param[in]  owner    Clé publique du propriétaire
 * @param[in]  dag      Référence au DAG
 * @param[in]  get_time Fonction de récupération du temps (NULL interdit)
 * @return ESP_OK en cas de succès
 */
esp_err_t wallet_init(wallet_t *wallet, const public_key_t *owner,
                      dag_t *dag, get_time_ms_fn get_time);

/**
 * @brief Calcule le solde disponible (non verrouillé) du wallet.
 *
 * Parcourt le DAG et accumule :
 * - Les crédits reçus (MINT vers owner, TRANSFER vers owner, status CONFIRMED)
 * - Moins les débits émis (TRANSFER depuis owner, status CONFIRMED ou LOCKED)
 *   incluant les frais de transfert (amount + tx->fee)
 *
 * Le solde verrouillé (LOCKED) est compté comme dépensé — il n'est
 * pas disponible pour un nouveau paiement.
 *
 * Les frais sont redirigés vers le fee_recipient du wallet s'il est
 * configuré (non-zéro). Sinon, ils sont brûlés (disparaissent de la
 * masse monétaire). L'émetteur paie amount + fee, le destinataire
 * ne reçoit que amount. Le fee est crédité au fee_recipient ou détruit.
 * Le fee est stocké dans chaque transaction au moment de sa création,
 * garantissant la cohérence même si le taux change.
 *
 * @param[in]  wallet          Wallet source
 * @param[in]  base_balance    Solde de base (issu du dernier checkpoint, 0 si aucun)
 * @param[out] available       Solde disponible résultant
 * @return ESP_OK en cas de succès
 */
esp_err_t wallet_get_balance(const wallet_t *wallet, uint32_t base_balance,
                             uint32_t *available);

/**
 * @brief Calcule le total des crédits créés (MINT confirmés) dans le DAG.
 *
 * Parcourt le DAG et somme les montants de toutes les TX MINT CONFIRMED.
 * Utilisé pour vérifier le plafond max_supply de la monnaie.
 *
 * @param[in]  dag          DAG source
 * @param[out] total_minted Total des montants MINT confirmés
 * @return ESP_OK en cas de succès
 */
esp_err_t wallet_get_total_minted(const dag_t *dag, uint64_t *total_minted);

/**
 * @brief [C2-fix] Calcule le solde d'une pubkey ARBITRAIRE (pas juste
 *        l'owner du wallet) à partir du checkpoint + DAG courant.
 *
 * Utilisée pour vérifier le solde de `tx->from` d'une transaction reçue
 * du réseau (validation currency côté récepteur). Logique identique à
 * wallet_get_balance mais avec une pubkey arbitraire comme cible et une
 * fee_recipient key explicite.
 *
 * ⚠ LIMITE CONNUE : le solde calculé reflète UNIQUEMENT les TX observées
 * dans notre état local (checkpoint + DAG). Si la pubkey cible a émis des
 * TX qu'on n'a pas encore reçues, on sous-estime ses dépenses. Ce n'est
 * donc pas une garantie forte anti-double-dépense — c'est une défense en
 * profondeur. La garantie principale reste :
 *  - lock source côté émetteur (wallet_lock) ;
 *  - détection de conflits via nonce monotone (dag_merge + tx.seq).
 *
 * @param[in]  dag            DAG source (transactions post-checkpoint)
 * @param[in]  checkpoint     Checkpoint (peut être NULL — base=0)
 * @param[in]  target         Clé publique dont on veut le solde
 * @param[in]  fee_recipient  Clé publique du recipient des fees (peut être
 *                            NULL ou zéro — fees brûlés)
 * @param[out] balance        Solde calculé
 * @return ESP_OK en cas de succès
 */
esp_err_t wallet_get_balance_for(const dag_t *dag,
                                 const checkpoint_t *checkpoint,
                                 const public_key_t *target,
                                 const public_key_t *fee_recipient,
                                 uint32_t *balance);

#endif /* WALLET_H */
