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
 */
void load_next_seq_or_recompute(void);

#ifdef __cplusplus
}
#endif

#endif /* MESHPAY_PERSISTENCE_NVS_NEXT_SEQ_H */
