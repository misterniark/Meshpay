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

#ifdef MP_HAS_LORA

uint64_t get_lamport_wrapper(void)
{
    return time_manager_get_lamport(&s_time_manager);
}

uint32_t main_collect_confirmed_txs(uint64_t       since_ts,
                                    transaction_t *out_buf,
                                    uint32_t       max_count,
                                    uint64_t      *out_newest_ts,
                                    void          *ctx)
{
    (void)ctx;
    if (!out_buf || !out_newest_ts || max_count == 0) {
        if (out_newest_ts) *out_newest_ts = since_ts;
        return 0;
    }

    uint64_t newest  = since_ts;
    uint32_t written = 0;

    /*
     * Timeout 1 s (conserve par parallelisme avec l'ancien code) :
     * si core_task tient le mutex trop longtemps, on saute ce cycle ;
     * le prochain reessaiera.
     */
    if (xSemaphoreTake(s_state_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        ESP_LOGW(TAG, "Sync LoRa : impossible d'acquerir s_state_mutex, "
                      "cycle saute");
        *out_newest_ts = newest;
        return 0;
    }

    for (uint32_t i = 0; i < s_dag.count && written < max_count; i++) {
        const transaction_t *tx = &s_dag.transactions[i];
        if (tx->status == TX_STATUS_CONFIRMED && tx->timestamp > since_ts) {
            memcpy(&out_buf[written], tx, sizeof(transaction_t));
            if (tx->timestamp > newest) newest = tx->timestamp;
            written++;
        }
    }

    xSemaphoreGive(s_state_mutex);

    *out_newest_ts = newest;
    return written;
}

#endif /* MP_HAS_LORA */
