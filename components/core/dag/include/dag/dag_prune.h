/**
 * @file dag_prune.h
 * @brief Élagage du DAG (suppression des transactions anciennes).
 *
 * Quand la fenêtre glissante atteint son seuil (80% de
 * DAG_MAX_TRANSACTIONS = 250 — soit 200 TX), un checkpoint est créé
 * par le module wallet et les transactions antérieures sont purgées.
 * Ce module fournit les fonctions de purge.
 *
 * [F-DG-012] La valeur historique "500 TX" mentionnée dans ce
 * commentaire avant 2026-05-15 était périmée : le seuil a été réduit
 * à 250 pour tenir dans la DRAM ESP32 partagée avec LVGL + Wi-Fi.
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
 * Retourne true si le nombre de transactions a atteint 80% de
 * DAG_MAX_TRANSACTIONS (la marge de 20% laisse le temps de créer
 * et de sauvegarder le checkpoint avant que le DAG ne sature
 * complètement).
 *
 * @param[in] dag DAG à vérifier
 * @return true si le DAG atteint le seuil et nécessite un checkpoint
 */
bool dag_needs_checkpoint(const dag_t *dag);

#endif /* DAG_PRUNE_H */
