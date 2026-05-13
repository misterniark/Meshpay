/**
 * @file stack_monitor.c
 * @brief Implementation du monitoring de stack (voir stack_monitor.h).
 */

#include "stack_monitor.h"

#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "app_state.h"

static const char *TAG = "stkmon";

static const char *const s_monitored_tasks[] = {
    "espnow", "lora", "core", "ui", "stkmon", NULL,
};

void stack_monitor_task(void *param)
{
    (void)param;
    const TickType_t period = pdMS_TO_TICKS(STACK_MONITOR_PERIOD_MS);
    for (;;) {
        vTaskDelay(period);
        for (int i = 0; s_monitored_tasks[i]; i++) {
            /*
             * Skip notre propre tache (Lot E.6) : auto-mesurer
             * uxTaskGetStackHighWaterMark sur soi-meme + ESP_LOGI
             * consomme une portion non negligeable de la stack et
             * peut declencher un overflow sur cette tache deja
             * dimensionnee au plus juste. La HWM des autres taches
             * suffit pour le diagnostic d'overflow.
             */
            if (strcmp(s_monitored_tasks[i], "stkmon") == 0) continue;

            TaskHandle_t h = xTaskGetHandle(s_monitored_tasks[i]);
            if (!h) continue;  /* tache non lancee (selon target) */
            UBaseType_t hwm_words = uxTaskGetStackHighWaterMark(h);
            ESP_LOGI(TAG, "Stack HWM %-7s : %u mots libres",
                     s_monitored_tasks[i], (unsigned)hwm_words);
        }
    }
}
