/**
 * @file lora_frag.h
 * @brief Fragmentation et réassemblage de paquets LoRa.
 *
 * Quand les données dépassent la taille max d'un paquet LoRa (255 octets),
 * elles sont découpées en fragments avec un header de 4 octets :
 *
 *   [0x11] [fragment_index] [total_fragments] [sequence_id]
 *
 * - fragment_index : 0 à total_fragments-1
 * - total_fragments : nombre total de fragments
 * - sequence_id : identifiant unique de la séquence (0-255)
 *
 * Côté émetteur : lora_frag_split() découpe un buffer en paquets.
 * Côté récepteur : lora_frag_receive() accumule les fragments et
 * signale quand tous sont reçus.
 *
 * Timeout de 10 secondes pour abandonner un réassemblage incomplet.
 * Maximum 16 fragments → ~4 KB de données max.
 *
 * Ce module est du C pur sans dépendance OS (pas de FreeRTOS).
 * Le temps est injecté en paramètre pour la testabilité.
 */

#ifndef LORA_FRAG_H
#define LORA_FRAG_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/** Taille du header de fragmentation */
#define LORA_FRAG_HEADER_SIZE   4

/** Taille max d'un paquet LoRa complet */
#define LORA_FRAG_PACKET_MAX    255

/** Payload max par fragment (255 - 4 = 251 octets) */
#define LORA_FRAG_PAYLOAD_MAX   (LORA_FRAG_PACKET_MAX - LORA_FRAG_HEADER_SIZE)

/** Nombre maximum de fragments par séquence */
#define LORA_FRAG_MAX_FRAGMENTS 16

/** Timeout de réassemblage en millisecondes */
#define LORA_FRAG_TIMEOUT_MS    10000

/* ================================================================
 * Fragmentation (côté émetteur)
 * ================================================================ */

/**
 * Découper un buffer de données en fragments LoRa.
 *
 * Chaque fragment est un paquet complet avec header 4 octets + payload.
 * Les paquets sont écrits dans le tableau `packets`.
 *
 * @param data         Données à fragmenter
 * @param data_len     Taille des données
 * @param seq_id       Identifiant de séquence (0-255)
 * @param packets      [out] Tableau de paquets (chaque entrée = LORA_FRAG_PACKET_MAX octets)
 * @param packet_lens  [out] Taille réelle de chaque paquet
 * @param packet_count [out] Nombre de paquets générés
 * @return 0 en cas de succès, -1 si données trop volumineuses ou paramètre NULL
 */
int lora_frag_split(const uint8_t *data, size_t data_len,
                    uint8_t seq_id,
                    uint8_t packets[][LORA_FRAG_PACKET_MAX],
                    size_t *packet_lens,
                    uint8_t *packet_count);

/* ================================================================
 * Réassemblage (côté récepteur)
 * ================================================================ */

/**
 * Contexte de réassemblage d'une séquence de fragments.
 *
 * Un seul contexte suffit car on ne reçoit qu'une séquence à la fois
 * dans le cas normal. Si un nouveau seq_id arrive alors qu'un
 * réassemblage est en cours, l'ancien est abandonné.
 */
typedef struct {
    uint8_t  seq_id;           /* ID de la séquence en cours */
    uint8_t  total_fragments;  /* Nombre total attendu */
    uint16_t received_mask;    /* Bitmask des fragments reçus (bit N = fragment N) */
    uint8_t  buffer[LORA_FRAG_MAX_FRAGMENTS * LORA_FRAG_PAYLOAD_MAX]; /* Buffer réassemblé */
    size_t   fragment_lens[LORA_FRAG_MAX_FRAGMENTS]; /* Taille de chaque fragment reçu */
    uint64_t start_time;       /* Timestamp du premier fragment reçu */
    bool     active;           /* true si un réassemblage est en cours */
} lora_frag_ctx_t;

/**
 * Initialiser le contexte de réassemblage.
 *
 * @param ctx Contexte à initialiser
 */
void lora_frag_ctx_init(lora_frag_ctx_t *ctx);

/**
 * Soumettre un fragment reçu pour réassemblage.
 *
 * Si le seq_id est différent de celui en cours, l'ancien réassemblage
 * est abandonné et un nouveau commence.
 *
 * @param ctx            Contexte de réassemblage
 * @param frag_index     Index du fragment (0 à total-1)
 * @param total          Nombre total de fragments
 * @param seq_id         Identifiant de séquence
 * @param payload        Données du fragment (sans header)
 * @param payload_len    Taille du payload
 * @param current_time   Timestamp courant en ms (injecté pour testabilité)
 * @return true si TOUS les fragments sont reçus (réassemblage complet)
 */
bool lora_frag_receive(lora_frag_ctx_t *ctx,
                       uint8_t frag_index, uint8_t total,
                       uint8_t seq_id,
                       const uint8_t *payload, size_t payload_len,
                       uint64_t current_time);

/**
 * Extraire le buffer réassemblé complet.
 *
 * Appelable uniquement après que lora_frag_receive() a retourné true.
 * Concatène les fragments dans l'ordre (0, 1, 2, ...) dans out_buf.
 *
 * @param ctx         Contexte avec réassemblage complet
 * @param out_buf     [out] Buffer de destination
 * @param out_buf_len Taille du buffer de destination
 * @param out_len     [out] Nombre d'octets écrits
 * @return 0 en cas de succès, -1 si réassemblage incomplet ou buffer trop petit
 */
int lora_frag_get_result(const lora_frag_ctx_t *ctx,
                         uint8_t *out_buf, size_t out_buf_len,
                         size_t *out_len);

/**
 * Vérifier et expirer les réassemblages en cours dépassant le timeout.
 *
 * @param ctx          Contexte à vérifier
 * @param current_time Timestamp courant en ms
 */
void lora_frag_expire(lora_frag_ctx_t *ctx, uint64_t current_time);

#endif /* LORA_FRAG_H */
