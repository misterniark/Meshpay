/**
 * @file dag_prune.h
 * @brief Élagage du DAG (suppression des transactions anciennes).
 *
 * Quand la fenêtre glissante est pleine (500 TX), un checkpoint est créé
 * (par le module wallet) et les transactions antérieures sont purgées.
 * Ce module fournit les fonctions de purge.
 */

#ifndef DAG_PRUNE_H
#define DAG_PRUNE_H

#include "dag/dag.h"
#include "crypto/crypto_types.h"
#include "esp_err.h"

/**
 * @brief Purge toutes les transactions plus anciennes qu'un timestamp donné.
 *
 * Supprime du DAG toutes les transactions dont le timestamp est inférieur
 * ou égal à before_timestamp. Les transactions restantes sont compactées
 * en début de tableau.
 *
 * Attention : après un élagage, les tips et les références de parents
 * peuvent devenir invalides. Le wallet doit s'appuyer sur le checkpoint
 * pour les soldes antérieurs.
 *
 * @param[in,out] dag              DAG à élaguer
 * @param[in]     before_timestamp Timestamp limite (inclus) — tout ce qui est
 *                                 <= à ce timestamp est supprimé
 * @return ESP_OK en cas de succès
 */
esp_err_t dag_prune_before(dag_t *dag, uint64_t before_timestamp);

/**
 * @brief Purge toutes les transactions du DAG.
 *
 * Remet le DAG à zéro. Utilisé après un checkpoint complet
 * quand on veut repartir d'une fenêtre vide.
 *
 * @param[in,out] dag DAG à vider
 * @return ESP_OK en cas de succès
 */
esp_err_t dag_prune_all(dag_t *dag);

/**
 * @brief Vérifie si le DAG a besoin d'un checkpoint.
 *
 * Retourne true si le nombre de transactions a atteint le seuil
 * de 500 (taille de la fenêtre glissante).
 *
 * @param[in] dag DAG à vérifier
 * @return true si le DAG est plein et nécessite un checkpoint
 */
bool dag_needs_checkpoint(const dag_t *dag);

#endif /* DAG_PRUNE_H */
