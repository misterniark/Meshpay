/**
 * @file dag_glue.h
 * @brief Insertion DAG avec checkpoint automatique.
 *
 * `dag_insert_and_track` insere une TX puis appelle
 * `auto_checkpoint_if_needed` qui declenche la creation d'un checkpoint
 * + l'elagage du DAG quand celui-ci atteint son seuil (80% capacite).
 *
 * ORDRE DE VERROUILLAGE [F-DG-004] — A RESPECTER :
 *
 *   s_state_mutex (mutex applicatif, defini dans app_state.c)
 *      └─> dag->mutex   (mutex recursif interne au DAG)
 *
 * Toutes ces fonctions DOIVENT etre appelees sous s_state_mutex.
 * Les fonctions DAG (dag_insert, dag_merge_transaction, dag_prune_*,
 * etc.) prennent leur propre mutex interne en plus. L'ordre
 * d'acquisition fixe (s_state d'abord, dag->mutex ensuite) garantit
 * l'absence d'interblocage tant qu'aucun autre chemin n'inverse
 * cet ordre.
 *
 * Si vous ajoutez un nouveau chemin d'acces concurrent au DAG (tache
 * en plus, ISR, callback HAL), VERIFIEZ que cet ordre est respecte
 * et ne JAMAIS prendre s_state_mutex en tenant deja dag->mutex.
 */

#ifndef MESHPAY_DAG_GLUE_H
#define MESHPAY_DAG_GLUE_H

#include "esp_err.h"
#include "transaction/tx_types.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t dag_insert_and_track(const transaction_t *tx);
void      auto_checkpoint_if_needed(void);

#ifdef __cplusplus
}
#endif

#endif /* MESHPAY_DAG_GLUE_H */
