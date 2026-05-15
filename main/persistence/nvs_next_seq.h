/**
 * @file persistence/nvs_next_seq.h
 * @brief [I3-fix] Nonce monotone par emetteur, persiste en NVS.
 *
 * Le seq attribue a une TX sortante doit etre strictement croissant
 * pour cette pubkey. Si on perd la valeur (reset NVS), on la recalcule
 * comme `1 + max(seq des TX ou from == self)` en parcourant le DAG.
 * Cela garantit qu'on ne reutilisera jamais un seq deja diffuse.
 *
 * Strategie de persistance : ecrire en NVS AVANT d'utiliser la valeur,
 * pour qu'en cas de crash apres ecriture mais avant emission, le seq
 * soit simplement "perdu" (saute un numero). Mieux vaut un trou dans
 * la sequence qu'un conflit apres reboot.
 */

#ifndef MESHPAY_PERSISTENCE_NVS_NEXT_SEQ_H
#define MESHPAY_PERSISTENCE_NVS_NEXT_SEQ_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Retourne le prochain seq a utiliser et persiste l'increment.
 * Appelee sous s_state_mutex.
 */
uint32_t next_seq(void);

/**
 * @brief Charge `s_next_seq` depuis NVS, ou le recalcule depuis le DAG.
 *        A appeler une fois au boot, apres init du DAG.
 *
 * [F-DG-011] Stratégie de recovery hybride (audit 2026-05-15) :
 *   1. NVS_KEY_NEXT_SEQ (clé légère mise à jour à chaque next_seq).
 *   2. Si échec : DAG local (`max(seq) + 1` sur les TX `from==self`).
 *   3. Si DAG vide : NVS_KEY_OWN_MAX_SEQ (sauvegarde à chaque checkpoint).
 *   4. Si tout échoue : log critique + s_next_seq = 0 + seq_recovery_failed=true
 *      (l'UI peut afficher un avertissement).
 */
void load_next_seq_or_recompute(void);

/**
 * @brief Persiste le max(seq) du propriétaire au moment du checkpoint.
 *
 * [F-DG-011] À appeler après chaque création/sauvegarde de checkpoint.
 * Inspecte le DAG (TX `from == self`) pour identifier le `max(seq)` et
 * l'écrit en NVS sous `NVS_KEY_OWN_MAX_SEQ`. Cette valeur sert de filet
 * de sauvegarde au boot suivant si NVS_KEY_NEXT_SEQ est perdu et que
 * le DAG est vide (purge totale).
 *
 * Appelée sous s_state_mutex (depuis auto_checkpoint_if_needed).
 */
void nvs_persist_own_max_seq(void);

/**
 * @brief Indique si la récupération du next_seq au boot a échoué.
 *
 * [F-DG-011] Retourne true si load_next_seq_or_recompute a dû
 * fall-back à s_next_seq=0 sans pouvoir confirmer qu'aucun seq
 * n'avait jamais été émis. Dans ce cas, le device risque d'être
 * banni par ses peers s'il tente d'émettre une TX. L'UI peut
 * utiliser cette information pour afficher un avertissement.
 */
bool seq_recovery_failed(void);

#ifdef __cplusplus
}
#endif

#endif /* MESHPAY_PERSISTENCE_NVS_NEXT_SEQ_H */
