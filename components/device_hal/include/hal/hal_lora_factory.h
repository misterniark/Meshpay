/**
 * @file hal_lora_factory.h
 * @brief Factory unifiee de creation du HAL LoRa.
 *
 * Point d'entree unique pour le code applicatif : il appelle
 * hal_lora_create_default() sans connaitre le module radio concret.
 * Le choix Wio-E5 / Core1262 se fait par l'option Kconfig
 * CONFIG_MESHPAY_LORA_DRIVER ; les GPIO/UART sont lus depuis Kconfig.
 *
 * C'est l'unique fichier qui connait les details materiels de chaque
 * puce : ajouter un 3e driver demain ne touche que ce fichier.
 */

#ifndef HAL_LORA_FACTORY_H
#define HAL_LORA_FACTORY_H

#include "hal/hal_lora.h"

/**
 * Cree l'instance LoRa correspondant au driver selectionne en Kconfig.
 *
 * @param lora [out] Vtable a remplir
 * @return HAL_OK en cas de succes, HAL_ERR_INVALID si lora == NULL,
 *         ou le code d'erreur de la factory concrete.
 */
hal_err_t hal_lora_create_default(hal_lora_t *lora);

#endif /* HAL_LORA_FACTORY_H */
