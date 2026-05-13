/**
 * @file time_glue.c
 * @brief Implementation des wrappers temps (voir time_glue.h).
 */

#include "time_glue.h"

#include <string.h>
#include <inttypes.h>

#include "esp_log.h"
#include "esp_timer.h"

#include "currency/currency_melt.h"
#include "transaction/tx_types.h"

static const char *TAG = "time_glue";

uint64_t platform_get_monotonic_ms(void)
{
    return (uint64_t)(esp_timer_get_time() / 1000);
}

uint64_t get_time_ms_wrapper(void)
{
    return time_manager_get_monotonic(&s_time_manager);
}

uint64_t get_tx_timestamp_wrapper(void)
{
    return time_manager_get_tx_timestamp(&s_time_manager);
}

uint32_t apply_pending_melt(uint32_t *balance)
{
    if (!s_currency.melt_enabled) {
        return 0;
    }

    /* La fonte requiert un temps fiable (mode MASTER). */
    if (s_time_manager.mode != TIME_MODE_MASTER ||
        !time_manager_has_valid_master(&s_time_manager)) {
        return 0;
    }

    uint64_t now = get_time_ms_wrapper();
    uint32_t ticks = currency_melt_ticks_due(&s_currency,
                                              s_wallet.last_melt_timestamp,
                                              now);
    if (ticks == 0) {
        return 0;
    }

    *balance = currency_melt_apply(&s_currency, *balance, ticks);
    s_wallet.last_melt_timestamp = currency_melt_next_timestamp(
        &s_currency, s_wallet.last_melt_timestamp, ticks, now);

    ESP_LOGI(TAG, "Fonte appliquee: %"PRIu32" ticks, nouveau solde=%"PRIu32,
             ticks, *balance);

    return ticks;
}

uint32_t compute_melted_balance(uint32_t balance)
{
    if (!s_currency.melt_enabled) {
        return balance;
    }
    if (s_time_manager.mode != TIME_MODE_MASTER ||
        !time_manager_has_valid_master(&s_time_manager)) {
        return balance;
    }

    uint64_t now = get_time_ms_wrapper();
    uint32_t ticks = currency_melt_ticks_due(&s_currency,
                                              s_wallet.last_melt_timestamp,
                                              now);
    if (ticks == 0) {
        return balance;
    }
    return currency_melt_apply(&s_currency, balance, ticks);
}

/* Les wrappers LoRa (get_lamport_wrapper, main_collect_confirmed_txs)
 * sont desormais prives a transport/transport_lora.c (Lot D.3). */
