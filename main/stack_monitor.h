/**
 * @file stack_monitor.h
 * @brief Tache FreeRTOS de monitoring des high-water-marks de stack.
 *
 * Loggue toutes les STACK_MONITOR_PERIOD_MS la marge restante (HWM) de
 * chaque tache critique. Permet d'ajuster les tailles de stack en
 * production sans reflasher un build instrumente : une marge < 512 mots
 * signale un risque d'overflow.
 *
 * Note : uxTaskGetStackHighWaterMark retourne le MINIMUM de mots libres
 * observe depuis le boot — c'est donc une valeur monotone decroissante
 * dans le pire cas.
 */

#ifndef MESHPAY_STACK_MONITOR_H
#define MESHPAY_STACK_MONITOR_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Tache de monitoring (a passer a xTaskCreate).
 *
 * Boucle infinie : delay periodique puis log du HWM de chaque tache
 * surveillee. Saute sa propre auto-mesure (Lot E.6) car
 * uxTaskGetStackHighWaterMark + ESP_LOGI sur soi-meme peut declencher
 * un overflow sur cette tache dimensionnee au plus juste.
 */
void stack_monitor_task(void *param);

#ifdef __cplusplus
}
#endif

#endif /* MESHPAY_STACK_MONITOR_H */
