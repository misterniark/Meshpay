/**
 * @file persistence/nvs_next_seq.c
 * @brief Implementation nonce monotone (voir header).
 */

#include "nvs_next_seq.h"

#include <inttypes.h>

#include "esp_log.h"

#include "app_state.h"
#include "transaction/tx_types.h"

static const char *TAG = "nvs_seq";

uint32_t next_seq(void)
{
    uint32_t seq = s_next_seq;
    s_next_seq++;
    hal_err_t herr = s_storage.u32_write(NVS_NAMESPACE, NVS_KEY_NEXT_SEQ,
                                         s_next_seq, s_storage.ctx);
    if (herr != HAL_OK) {
        /*
         * Echec persistance : on continue quand meme. La persistance
         * protege contre les reboots, pas contre une corruption flash.
         */
        ESP_LOGW(TAG, "next_seq: echec persistance NVS (%d)", herr);
    }
    return seq;
}

void load_next_seq_or_recompute(void)
{
    uint32_t persisted = 0;
    hal_err_t herr = s_storage.u32_read(NVS_NAMESPACE, NVS_KEY_NEXT_SEQ,
                                        &persisted, s_storage.ctx);
    if (herr == HAL_OK) {
        s_next_seq = persisted;
        ESP_LOGI(TAG, "next_seq charge depuis NVS: %"PRIu32, s_next_seq);
        return;
    }

    /* Reset NVS ou premier boot : reconstituer depuis le DAG. */
    uint32_t max_seq = 0;
    for (uint32_t i = 0; i < s_dag.count; i++) {
        const transaction_t *tx = &s_dag.transactions[i];
        if (public_key_equal(&tx->from, &s_keypair.public_key) &&
            tx->seq > max_seq) {
            max_seq = tx->seq;
        }
    }
    s_next_seq = (max_seq == 0) ? 0 : (max_seq + 1);
    ESP_LOGI(TAG, "next_seq recompute depuis DAG: %"PRIu32, s_next_seq);
}
