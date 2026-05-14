/**
 * @file test_hal_power.c
 * @brief Tests de l'impl stub de hal_power.
 */

#include "unity.h"
#include "hal/hal_power.h"

TEST_CASE("hal_power_stub_create_returns_ok", "[hal_power]")
{
    hal_power_t power;
    hal_err_t err = hal_power_stub_create(&power);
    TEST_ASSERT_EQUAL(HAL_OK, err);
    TEST_ASSERT_NOT_NULL(power.get_source);
}

TEST_CASE("hal_power_stub_always_usb", "[hal_power]")
{
    hal_power_t power;
    hal_power_stub_create(&power);
    TEST_ASSERT_EQUAL(POWER_SOURCE_USB, power.get_source(power.ctx));
}
