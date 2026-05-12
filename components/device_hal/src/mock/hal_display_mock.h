/**
 * @file hal_display_mock.h
 * @brief Mock display pour tests — toutes les opérations sont no-op.
 *
 * Retourne HAL_OK pour toutes les opérations sans effet de bord.
 * touch_read retourne toujours pressed=false.
 * get_resolution retourne 320x240 (résolution CYD par défaut).
 */

#ifndef HAL_DISPLAY_MOCK_H
#define HAL_DISPLAY_MOCK_H

#include "hal/hal_display.h"

/**
 * Créer une instance mock de l'affichage.
 *
 * @param display [out] Vtable à remplir
 * @return HAL_OK
 */
hal_err_t hal_display_mock_create(hal_display_t *display);

#endif /* HAL_DISPLAY_MOCK_H */
