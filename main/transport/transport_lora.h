/**
 * @file transport/transport_lora.h
 * @brief Facade LoRa : API uniforme sur toutes les cibles.
 *
 * **Pourquoi cette couche ?**
 *
 * Avant le Lot D.3, les handlers et la core_task etaient parsemes de
 * `#ifdef MP_HAS_LORA ... s_lora_hal.send(...) ... #endif`. Sur les
 * cibles sans Wio-E5, ces blocs etaient des trous beants dans la logique.
 *
 * Cette facade fournit l'API LoRa **comme si LoRa etait toujours present** :
 * deux implementations alternatives, selectionnees par CMake :
 *
 *   - `transport_lora.c`      : impl reelle (cibles avec Wio-E5 :
 *                               ESP32 CYD + ESP32-S3 Waveshare)
 *   - `transport_lora_stub.c` : no-op (eventuelles cibles sans Wio-E5)
 *
 * Le code applicatif n'a plus aucun `#ifdef` autour des appels LoRa.
 *
 * **Ownership des buffers de relay/pong**
 *
 * Les buffers (s_relay_bcast_buf, s_relay_ping_buf, s_pong_buf) sont
 * desormais internes a `transport_lora.c`. Les handlers leur passent une
 * copie via les fonctions `queue_*`. Sur le stub, ces fonctions sont
 * des no-ops — pas de buffer, pas de RAM gachee sur les cibles sans LoRa.
 */

#ifndef MESHPAY_TRANSPORT_LORA_H
#define MESHPAY_TRANSPORT_LORA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "hal/hal_storage.h"  /* pour hal_err_t */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Indique si LoRa est physiquement present (vrai sur ESP32 CYD).
 *
 * Utile pour quelques branches de UI/logs ou la difference reste
 * pertinente (ex : afficher "LoRa OK" vs "pas de LoRa").
 */
bool transport_lora_available(void);

/**
 * @brief Initialise le HAL LoRa et lance lora_sync_task.
 *
 * Sur stub : ne fait rien, retourne HAL_OK.
 */
hal_err_t transport_lora_init_and_start(void);

/**
 * @brief Envoi LoRa synchrone d'un buffer pre-packe.
 *
 * @param what Etiquette pour les logs (ex: "broadcast", "ping", "pong",
 *             "attestation"). Sur stub : no-op silencieux.
 * @return true si envoi reussi (false sur stub ou echec HAL).
 */
bool transport_lora_send(const uint8_t *buf, size_t len, const char *what);

/**
 * @brief File un broadcast pour relay (appele sous s_state_mutex).
 *
 * Le buffer est copie. L'envoi reel se fait dans `transport_lora_pump()`
 * apres un delai aleatoire (200-1000 ms) anti-collision.
 *
 * Sur stub : no-op (rien a relayer si pas de LoRa).
 */
void transport_lora_queue_relay_broadcast(const uint8_t *buf, size_t len);

/**
 * @brief File un PING pour relay (meme principe que relay broadcast).
 */
void transport_lora_queue_relay_ping(const uint8_t *buf, size_t len);

/**
 * @brief File un PONG pour envoi differe.
 *
 * @param delay_ms Delai aleatoire (1-5 s typiquement) avant emission.
 *                 Permet d'eviter que tous les devices repondent au
 *                 meme instant.
 */
void transport_lora_queue_pong_delayed(const uint8_t *buf, size_t len, uint32_t delay_ms);

/**
 * @brief A appeler depuis core_task apres release du mutex.
 *
 * Verifie chaque file (relay broadcast/ping, pong differe) et envoie
 * les messages dont l'echeance est atteinte. Sur stub : no-op.
 */
void transport_lora_pump(void);

/**
 * @brief Regle l'intervalle de sync LoRa (ms).
 *
 * Permet au power_manager de ralentir la sync en mode ECO. Sur le stub
 * (cibles sans LoRa) : no-op. Sur l'impl reelle : memorise la valeur ;
 * elle sera prise en compte au prochain cycle de lora_sync_task.
 *
 * NB : aujourd'hui inerte sur ESP32-S3 (lora_sync_task n'y tourne pas
 * encore) et non appele sur le CYD (power_manager y est le stub). Le
 * hook est pose pour le jour ou le S3 aura du LoRa.
 *
 * @param interval_ms Nouvel intervalle de sync en millisecondes.
 */
void transport_lora_set_sync_interval(uint32_t interval_ms);

#ifdef __cplusplus
}
#endif

#endif /* MESHPAY_TRANSPORT_LORA_H */
