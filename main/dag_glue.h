/**
 * @file dag_glue.h
 * @brief Insertion DAG avec checkpoint automatique.
 *
 * `dag_insert_and_track` insere une TX puis appelle
 * `auto_checkpoint_if_needed` qui declenche la creation d'un checkpoint
 * + l'elagage du DAG quand celui-ci atteint son seuil (80% capacite).
 *
 * Toutes ces fonctions DOIVENT etre appelees sous s_state_mutex.
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
