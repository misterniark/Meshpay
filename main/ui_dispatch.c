/**
 * @file ui_dispatch.c
 * @brief Implementation du dispatch UI (voir ui_dispatch.h).
 *
 * Appelee sous s_state_mutex dans core_task. Delegue chaque commande
 * aux ops correspondantes dans main/ops/.
 */

#include "ui_dispatch.h"

#include <inttypes.h>
#include <string.h>

#include "esp_log.h"

#include "app_state.h"  /* avant freertos/queue.h */
#include "freertos/queue.h"

#include "ops/ops.h"

static const char *TAG = "ui_disp";

void handle_ui_command(const ui_cmd_t *cmd)
{
    switch (cmd->type) {
        case UI_CMD_PAY: {
            ESP_LOGI(TAG, "UI CMD: Paiement %"PRIu32" vers peer",
                     cmd->data.pay.amount);
            esp_err_t pay_ret = initiate_payment(&cmd->data.pay.to,
                                                  cmd->data.pay.amount);
            if (pay_ret == ESP_OK) {
                s_pay_feedback = UI_PAY_FEEDBACK_OK;
            } else if (pay_ret == ESP_ERR_INVALID_STATE) {
                s_pay_feedback = UI_PAY_FEEDBACK_NO_FUNDS;
            } else {
                s_pay_feedback = UI_PAY_FEEDBACK_FAIL;
                ESP_LOGW(TAG, "Paiement echoue: %s", esp_err_to_name(pay_ret));
            }
            break;
        }

        case UI_CMD_MINT: {
            ESP_LOGI(TAG, "UI CMD: MINT %"PRIu32" credits",
                     cmd->data.mint.amount);
            esp_err_t mint_ret = initiate_mint(&cmd->data.mint.to,
                                                cmd->data.mint.amount);
            if (mint_ret != ESP_OK) {
                ESP_LOGW(TAG, "MINT echoue: %s", esp_err_to_name(mint_ret));
            }
            break;
        }

        case UI_CMD_DISCOVER_PEERS: {
            ESP_LOGI(TAG, "UI CMD: Discover peers");
            comm_cmd_t disc_cmd;
            memset(&disc_cmd, 0, sizeof(disc_cmd));
            disc_cmd.type = COMM_CMD_START_DISCOVER;
            xQueueSend(s_cmd_queue, &disc_cmd, pdMS_TO_TICKS(100));
            break;
        }

        case UI_CMD_BROADCAST_TEXT:
            ESP_LOGI(TAG, "UI CMD: Broadcast texte (%u chars)",
                     cmd->data.broadcast.text_len);
            /*
             * op_master compile partout : runtime check `is_master` +
             * transport_lora no-op sur cibles sans LoRa garantissent
             * un comportement coherent sans #ifdef.
             */
            broadcast_text_send(cmd->data.broadcast.text,
                                cmd->data.broadcast.text_len);
            break;

        case UI_CMD_PING:
            ESP_LOGI(TAG, "UI CMD: Ping");
            ping_send();
            break;

        case UI_CMD_SET_ALIAS:
            ESP_LOGI(TAG, "UI CMD: Set alias");
            set_alias_send(&cmd->data.set_alias.target,
                           cmd->data.set_alias.alias,
                           cmd->data.set_alias.alias_len);
            break;

        case UI_CMD_SET_BENEFICIARY:
            ESP_LOGI(TAG, "UI CMD: Set beneficiary");
            set_beneficiary_send(&cmd->data.set_beneficiary.target,
                                 &cmd->data.set_beneficiary.beneficiary,
                                 cmd->data.set_beneficiary.interval_min);
            break;

        default:
            ESP_LOGW(TAG, "UI CMD inconnue: %d", cmd->type);
            break;
    }
}
