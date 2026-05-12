/**
 * @file hal_display_ili9341.h
 * @brief Factory pour le driver ILI9341 + XPT2046 du CYD.
 *
 * Driver SPI pour l'ecran ILI9341 (240x320, 16-bit RGB565) et le
 * controleur tactile resistif XPT2046 du module CYD (ESP32).
 */

#ifndef HAL_DISPLAY_ILI9341_H
#define HAL_DISPLAY_ILI9341_H

#include "hal/hal_display.h"

/**
 * Créer une instance d'affichage pour ILI9341 + XPT2046.
 *
 * @param display [out] Vtable à remplir
 * @return HAL_OK en cas de succès
 */
hal_err_t hal_display_ili9341_create(hal_display_t *display);

#endif /* HAL_DISPLAY_ILI9341_H */
