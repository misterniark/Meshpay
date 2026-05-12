/**
 * @file hal_storage_mock.h
 * @brief Implémentation mock du stockage pour les tests unitaires.
 *
 * Stocke les données dans un tableau statique en mémoire RAM.
 * Aucune persistance réelle — les données disparaissent à la fin du test.
 *
 * Fonctions utilitaires exposées :
 * - hal_storage_mock_create() : factory pour remplir la vtable
 * - hal_storage_mock_reset()  : vider toutes les entrées (entre tests)
 * - hal_storage_mock_count()  : nombre d'entrées stockées (assertions)
 */

#ifndef HAL_STORAGE_MOCK_H
#define HAL_STORAGE_MOCK_H

#include "hal/hal_storage.h"

/**
 * Créer une instance mock du stockage.
 * Remplit la vtable avec les fonctions mock et initialise le contexte.
 *
 * @param storage [out] Vtable à remplir
 * @return HAL_OK toujours
 */
hal_err_t hal_storage_mock_create(hal_storage_t *storage);

/**
 * Vider toutes les entrées stockées.
 * À appeler entre chaque test pour un état propre.
 *
 * @param storage Instance mock créée par hal_storage_mock_create
 */
void hal_storage_mock_reset(hal_storage_t *storage);

/**
 * Obtenir le nombre d'entrées actuellement stockées.
 * Utile pour vérifier qu'un write a bien créé une entrée,
 * ou qu'un erase l'a bien supprimée.
 *
 * @param storage Instance mock
 * @return Nombre d'entrées
 */
uint32_t hal_storage_mock_count(const hal_storage_t *storage);

#endif /* HAL_STORAGE_MOCK_H */
