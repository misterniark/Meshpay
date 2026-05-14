/**
 * @file sx126x_hal_context.h
 * @brief Contexte materiel passe au pilote Semtech comme `context` opaque.
 *
 * Partage entre sx126x_hal.c (la glue) et hal_lora_core1262.c (le backend
 * qui cree le handle SPI et configure les GPIO). Le pilote Semtech reçoit
 * un `const void *context` qu'il transmet tel quel a la glue.
 */

#ifndef SX126X_HAL_CONTEXT_H
#define SX126X_HAL_CONTEXT_H

#include "driver/spi_master.h"

/** Ressources materielles necessaires aux transactions SX1262. */
typedef struct {
    spi_device_handle_t spi;        /**< Handle du device SPI (CS gere a la main) */
    int                 pin_nss;    /**< GPIO chip-select (pilote a la main) */
    int                 pin_busy;   /**< GPIO BUSY (entree) */
    int                 pin_reset;  /**< GPIO RESET (sortie) */
} core1262_hw_t;

#endif /* SX126X_HAL_CONTEXT_H */
