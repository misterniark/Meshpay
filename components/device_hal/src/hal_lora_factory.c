/**
 * @file hal_lora_factory.c
 * @brief Selection du backend HAL LoRa selon CONFIG_MESHPAY_LORA_DRIVER.
 *
 * Chaque branche #if lit les GPIO/UART depuis Kconfig et delegue a la
 * factory concrete du driver. Une seule branche est compilee.
 */

#include "hal/hal_lora_factory.h"
#include "sdkconfig.h"

#if defined(CONFIG_MESHPAY_LORA_DRIVER_WIO_E5)

#include "hal_lora_wio_e5.h"

hal_err_t hal_lora_create_default(hal_lora_t *lora)
{
    if (!lora) {
        return HAL_ERR_INVALID;
    }
    return hal_lora_wio_e5_create(lora,
                                  CONFIG_MESHPAY_LORA_WIOE5_UART_NUM,
                                  CONFIG_MESHPAY_LORA_WIOE5_PIN_TX,
                                  CONFIG_MESHPAY_LORA_WIOE5_PIN_RX);
}

#elif defined(CONFIG_MESHPAY_LORA_DRIVER_CORE1262)

/* Cette branche n'est compilee que lorsque le driver Core1262 est
 * selectionne (CONFIG_MESHPAY_LORA_DRIVER_CORE1262). */
#include "hal_lora_core1262.h"

hal_err_t hal_lora_create_default(hal_lora_t *lora)
{
    if (!lora) {
        return HAL_ERR_INVALID;
    }
    const hal_lora_core1262_pins_t pins = {
        .spi_host   = CONFIG_MESHPAY_LORA_C1262_SPI_HOST,
        .pin_sck    = CONFIG_MESHPAY_LORA_C1262_PIN_SCK,
        .pin_mosi   = CONFIG_MESHPAY_LORA_C1262_PIN_MOSI,
        .pin_miso   = CONFIG_MESHPAY_LORA_C1262_PIN_MISO,
        .pin_nss    = CONFIG_MESHPAY_LORA_C1262_PIN_NSS,
        .pin_reset  = CONFIG_MESHPAY_LORA_C1262_PIN_RESET,
        .pin_busy   = CONFIG_MESHPAY_LORA_C1262_PIN_BUSY,
        .pin_dio1   = CONFIG_MESHPAY_LORA_C1262_PIN_DIO1,
        .pin_rxen   = CONFIG_MESHPAY_LORA_C1262_PIN_RXEN,
        .pin_txen   = CONFIG_MESHPAY_LORA_C1262_PIN_TXEN,
    };
    return hal_lora_core1262_create(lora, &pins);
}

#else
#error "CONFIG_MESHPAY_LORA_DRIVER non defini — verifier components/device_hal/Kconfig"
#endif
