/**
 * @file hal_lora_wio_e5.h
 * @brief Factory pour le driver LoRa Grove Wio-E5 (UART/AT).
 *
 * Driver UART/AT pour le module LoRa Grove Wio-E5 (STM32WLE5JC).
 * Gere l'initialisation, l'envoi et la reception de paquets LoRa.
 */

#ifndef HAL_LORA_WIO_E5_H
#define HAL_LORA_WIO_E5_H

#include "hal/hal_lora.h"

/**
 * Créer une instance LoRa pour le module Grove Wio-E5.
 *
 * @param lora     [out] Vtable à remplir
 * @param uart_num Numéro du port UART (ex: UART_NUM_1)
 * @param tx_pin   GPIO TX
 * @param rx_pin   GPIO RX
 * @return HAL_OK en cas de succès
 */
hal_err_t hal_lora_wio_e5_create(hal_lora_t *lora,
                                 int uart_num, int tx_pin, int rx_pin);

#endif /* HAL_LORA_WIO_E5_H */
