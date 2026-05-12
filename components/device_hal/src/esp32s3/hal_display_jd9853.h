/**
 * @file hal_display_jd9853.h
 * @brief Factory pour le driver JD9853 (LCD) + AXS5106L (tactile)
 *        du Waveshare ESP32-S3-Touch-LCD-1.47.
 *
 * Implémentation complète : SPI LCD, I2C tactile, PWM rétroéclairage.
 * Toutes les opérations de la vtable hal_display_t sont fonctionnelles.
 */

#ifndef HAL_DISPLAY_JD9853_H
#define HAL_DISPLAY_JD9853_H

#include "hal/hal_display.h"

/**
 * Créer une instance d'affichage pour JD9853 + AXS5106L.
 *
 * Le Waveshare ESP32-S3-Touch-LCD-1.47 utilise :
 * - JD9853 sur SPI : 172×320 pixels, RGB565
 * - AXS5106L sur I2C : écran tactile capacitif
 * - Rétroéclairage contrôlé par PWM (LEDC) sur GPIO 46
 *
 * @param display [out] Vtable à remplir
 * @return HAL_OK en cas de succès
 */
hal_err_t hal_display_jd9853_create(hal_display_t *display);

#endif /* HAL_DISPLAY_JD9853_H */
