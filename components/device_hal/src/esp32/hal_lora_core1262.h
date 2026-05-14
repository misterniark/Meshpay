/**
 * @file hal_lora_core1262.h
 * @brief Factory du backend LoRa Waveshare Core1262 (SX1262 SPI).
 */

#ifndef HAL_LORA_CORE1262_H
#define HAL_LORA_CORE1262_H

#include "hal/hal_lora.h"

/** Brochage du module Core1262 cote ESP32 (issu de Kconfig). */
typedef struct {
    int spi_host;    /**< Hote SPI (ex: 2 pour SPI3_HOST) */
    int pin_sck;     /**< GPIO SCK */
    int pin_mosi;    /**< GPIO MOSI */
    int pin_miso;    /**< GPIO MISO */
    int pin_nss;     /**< GPIO NSS (chip-select) */
    int pin_reset;   /**< GPIO RESET */
    int pin_busy;    /**< GPIO BUSY */
    int pin_dio1;    /**< GPIO DIO1 (IRQ) */
    int pin_rxen;    /**< GPIO RXEN (switch RF RX) */
    int pin_txen;    /**< GPIO TXEN (switch RF TX) */
} hal_lora_core1262_pins_t;

/**
 * Cree une instance LoRa pour le module Waveshare Core1262.
 *
 * @param lora [out] Vtable a remplir
 * @param pins Brochage du module
 * @return HAL_OK en cas de succes, HAL_ERR_INVALID si un argument est NULL
 */
hal_err_t hal_lora_core1262_create(hal_lora_t *lora,
                                   const hal_lora_core1262_pins_t *pins);

#endif /* HAL_LORA_CORE1262_H */
