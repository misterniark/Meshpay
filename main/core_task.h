/**
 * @file core_task.h
 * @brief Tache FreeRTOS centrale qui orchestre tout le traitement metier.
 *
 * - Draine `s_evt_queue` (1 s timeout) → dispatch vers handlers/
 * - Draine `s_ui_cmd_queue` → dispatch vers ui_dispatch
 * - Tâches periodiques sous mutex : expiration des locks, auto-forward
 *   beneficiaire, application periodique de la fonte
 * - Hors mutex : transport_lora_pump() (relay broadcast/ping + PONG)
 */

#ifndef MESHPAY_CORE_TASK_H
#define MESHPAY_CORE_TASK_H

#ifdef __cplusplus
extern "C" {
#endif

/** Entree de la tache (a passer a xTaskCreate). */
void core_task(void *param);

#ifdef __cplusplus
}
#endif

#endif /* MESHPAY_CORE_TASK_H */
