/**
 * @file hal_display_mock.c
 * @brief Mock display — no-op pour tests headless.
 *
 * Toutes les opérations retournent HAL_OK sans effet.
 * Permet de tester les state machines UI sans écran physique.
 */

#include "hal_display_mock.h"
#include <string.h>

/** Résolution par défaut (CYD) */
#define MOCK_WIDTH  320
#define MOCK_HEIGHT 240

static hal_err_t mock_init(void *ctx)
{
    (void)ctx;
    return HAL_OK;
}

static hal_err_t mock_flush(uint16_t x1, uint16_t y1,
                            uint16_t x2, uint16_t y2,
                            const uint16_t *color, void *ctx)
{
    (void)x1; (void)y1; (void)x2; (void)y2; (void)color; (void)ctx;
    return HAL_OK;
}

static hal_err_t mock_touch_read(hal_touch_point_t *point, void *ctx)
{
    (void)ctx;
    if (point) {
        point->x = 0;
        point->y = 0;
        point->pressed = false;
    }
    return HAL_OK;
}

static hal_err_t mock_set_backlight(uint8_t brightness, void *ctx)
{
    (void)brightness; (void)ctx;
    return HAL_OK;
}

static hal_err_t mock_get_resolution(uint16_t *width, uint16_t *height,
                                     void *ctx)
{
    (void)ctx;
    if (width)  *width  = MOCK_WIDTH;
    if (height) *height = MOCK_HEIGHT;
    return HAL_OK;
}

hal_err_t hal_display_mock_create(hal_display_t *display)
{
    if (!display) {
        return HAL_ERR_INVALID;
    }

    display->init           = mock_init;
    display->flush          = mock_flush;
    display->touch_read     = mock_touch_read;
    display->set_backlight  = mock_set_backlight;
    display->get_resolution = mock_get_resolution;
    display->ctx            = NULL;

    return HAL_OK;
}
