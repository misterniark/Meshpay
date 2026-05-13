/**
 * @file handlers/handlers.h
 * @brief Declarations des handlers d'evenements `comm_event_t`.
 *
 * Chaque handler traite un type d'evenement remonte par espnow_task ou
 * lora_sync_task via `s_evt_queue`. Tous sont appeles depuis core_task
 * **sous s_state_mutex** (sauf mention contraire dans le commentaire).
 *
 * Implementations dans `handler_*.c` (Lot D.4).
 */

#ifndef MESHPAY_HANDLERS_H
#define MESHPAY_HANDLERS_H

#include "comm/comm_event.h"

#ifdef __cplusplus
extern "C" {
#endif

void handle_peer_discovered(const comm_event_t *evt);
void handle_tx_received(const comm_event_t *evt);
void handle_ack_received(const comm_event_t *evt);
void handle_tx_timeout(const comm_event_t *evt);
void handle_attestation_received(const comm_event_t *evt);
void handle_time_sync(const comm_event_t *evt);
void handle_broadcast_received(const comm_event_t *evt);
void handle_ping_received(const comm_event_t *evt);
void handle_pong_received(const comm_event_t *evt);
void handle_set_alias_received(const comm_event_t *evt);
void handle_set_beneficiary_received(const comm_event_t *evt);

/**
 * @brief Marque un PING comme deja vu dans le cache anti-boucle.
 *
 * Expose pour que `ping_send` (op maitre) puisse marquer son propre
 * PING avant emission — evite de le re-traiter si on recoit notre
 * propre relay LoRa.
 *
 * Appelee sous s_state_mutex.
 */
void ping_mark_seen_public(const public_key_t *master_key, uint16_t ping_id);

#ifdef __cplusplus
}
#endif

#endif /* MESHPAY_HANDLERS_H */
