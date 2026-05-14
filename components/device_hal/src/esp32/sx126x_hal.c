/**
 * @file sx126x_hal.c
 * @brief Glue plateforme ESP-IDF pour le pilote Semtech sx126x.
 *
 * Implemente les 4 fonctions sx126x_hal_* attendues par sx126x/sx126x.c :
 * transactions SPI (write/read), reset materiel, reveil de veille.
 *
 * Protocole SX126x : NSS bas, opcode + adresse + data, NSS haut. Avant
 * chaque transaction il faut attendre que BUSY repasse a 0. Le CS n'est
 * pas confie au pilote SPI (spics_io_num = -1 cote backend) : on le
 * pilote ici a la main pour englober opcode + data dans une seule
 * sequence NSS bas..haut.
 */

#include "sx126x_hal.h"
#include "sx126x_hal_context.h"

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_rom_sys.h"
#include "esp_log.h"

static const char *TAG = "sx126x_hal";

/** Timeout d'attente du pin BUSY (ms). */
#define BUSY_TIMEOUT_MS 100

/**
 * Attend que le SX1262 libere BUSY (niveau bas = pret).
 *
 * @param hw Contexte materiel
 * @return SX126X_HAL_STATUS_OK si BUSY est bas, _ERROR sur timeout
 */
static sx126x_hal_status_t wait_on_busy(const core1262_hw_t *hw)
{
    int waited_us = 0;
    while (gpio_get_level(hw->pin_busy) == 1) {
        esp_rom_delay_us(10);
        waited_us += 10;
        if (waited_us >= BUSY_TIMEOUT_MS * 1000) {
            ESP_LOGE(TAG, "Timeout BUSY (%d ms)", BUSY_TIMEOUT_MS);
            return SX126X_HAL_STATUS_ERROR;
        }
    }
    return SX126X_HAL_STATUS_OK;
}

sx126x_hal_status_t sx126x_hal_write(const void *context,
                                     const uint8_t *command,
                                     const uint16_t command_length,
                                     const uint8_t *data,
                                     const uint16_t data_length)
{
    const core1262_hw_t *hw = (const core1262_hw_t *)context;

    if (wait_on_busy(hw) != SX126X_HAL_STATUS_OK) {
        return SX126X_HAL_STATUS_ERROR;
    }

    /* NSS bas pour toute la transaction (opcode + data). */
    gpio_set_level(hw->pin_nss, 0);

    esp_err_t err = ESP_OK;

    /* 1. Envoi de l'opcode + adresse. */
    spi_transaction_t t_cmd = {
        .length    = (size_t)command_length * 8,
        .tx_buffer = command,
    };
    err = spi_device_polling_transmit(hw->spi, &t_cmd);

    /* 2. Envoi des donnees (si presentes). */
    if (err == ESP_OK && data_length > 0) {
        spi_transaction_t t_data = {
            .length    = (size_t)data_length * 8,
            .tx_buffer = data,
        };
        err = spi_device_polling_transmit(hw->spi, &t_data);
    }

    gpio_set_level(hw->pin_nss, 1);

    return (err == ESP_OK) ? SX126X_HAL_STATUS_OK : SX126X_HAL_STATUS_ERROR;
}

sx126x_hal_status_t sx126x_hal_read(const void *context,
                                    const uint8_t *command,
                                    const uint16_t command_length,
                                    uint8_t *data,
                                    const uint16_t data_length)
{
    const core1262_hw_t *hw = (const core1262_hw_t *)context;

    if (wait_on_busy(hw) != SX126X_HAL_STATUS_OK) {
        return SX126X_HAL_STATUS_ERROR;
    }

    gpio_set_level(hw->pin_nss, 0);

    esp_err_t err = ESP_OK;

    /* 1. Envoi de l'opcode + adresse (les octets de status sortis sont ignores). */
    spi_transaction_t t_cmd = {
        .length    = (size_t)command_length * 8,
        .tx_buffer = command,
    };
    err = spi_device_polling_transmit(hw->spi, &t_cmd);

    /* 2. Lecture des donnees : on cadence des octets factices et on capture MISO. */
    if (err == ESP_OK && data_length > 0) {
        spi_transaction_t t_data = {
            .length    = (size_t)data_length * 8,
            .rxlength  = (size_t)data_length * 8,
            .tx_buffer = NULL,           /* octets sortants = 0 (NOP) */
            .rx_buffer = data,
        };
        err = spi_device_polling_transmit(hw->spi, &t_data);
    }

    gpio_set_level(hw->pin_nss, 1);

    return (err == ESP_OK) ? SX126X_HAL_STATUS_OK : SX126X_HAL_STATUS_ERROR;
}

sx126x_hal_status_t sx126x_hal_reset(const void *context)
{
    const core1262_hw_t *hw = (const core1262_hw_t *)context;

    /* Reset materiel : RESET bas >= 100 us, puis haut, puis attente BUSY. */
    gpio_set_level(hw->pin_reset, 0);
    esp_rom_delay_us(200);
    gpio_set_level(hw->pin_reset, 1);
    vTaskDelay(pdMS_TO_TICKS(10));   /* laisser le SX1262 redemarrer */

    return wait_on_busy(hw);
}

sx126x_hal_status_t sx126x_hal_wakeup(const void *context)
{
    const core1262_hw_t *hw = (const core1262_hw_t *)context;

    /* Sortie de veille : un front descendant sur NSS reveille le SX1262. */
    gpio_set_level(hw->pin_nss, 0);
    esp_rom_delay_us(20);
    gpio_set_level(hw->pin_nss, 1);

    return wait_on_busy(hw);
}
