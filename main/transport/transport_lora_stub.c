/**
 * @file transport/transport_lora_stub.c
 * @brief Stub no-op de la facade LoRa (cibles sans Wio-E5).
 *
 * Compile sur les cibles qui n'embarquent pas de Wio-E5 — c.-a-d. ni
 * ESP32 CYD ni ESP32-S3 Waveshare (cf. CMakeLists). Aucune cible
 * supportee actuellement ne tombe dans ce cas ; le stub reste pour
 * d'eventuelles futures cibles sans LoRa.
 *
 * Aucun buffer, aucune logique : toutes les fonctions sont des no-ops.
 * Le code applicatif appelle la facade sans #ifdef ; sur ces cibles, ces
 * appels disparaissent (-flto les inline en rien) et le binaire ne paie
 * ni en RAM ni en flash pour le code LoRa.
 */

#include "transport_lora.h"

bool transport_lora_available(void)
{
    return false;
}

hal_err_t transport_lora_init_and_start(void)
{
    return HAL_OK;
}

bool transport_lora_send(const uint8_t *buf, size_t len, const char *what)
{
    (void)buf; (void)len; (void)what;
    return false;
}

void transport_lora_queue_relay_broadcast(const uint8_t *buf, size_t len)
{
    (void)buf; (void)len;
}

void transport_lora_queue_relay_ping(const uint8_t *buf, size_t len)
{
    (void)buf; (void)len;
}

void transport_lora_queue_pong_delayed(const uint8_t *buf, size_t len,
                                       uint32_t delay_ms)
{
    (void)buf; (void)len; (void)delay_ms;
}

void transport_lora_pump(void)
{
}

void transport_lora_set_sync_interval(uint32_t interval_ms)
{
    (void)interval_ms;
}
