/**
 * @file lora_frag.c
 * @brief Fragmentation et réassemblage de paquets LoRa.
 *
 * Implémentation du protocole de fragmentation :
 * - Split : découpe linéaire en morceaux de LORA_FRAG_PAYLOAD_MAX octets
 * - Receive : accumulation avec bitmask, supporte réception désordonnée
 * - Timeout : abandon automatique après LORA_FRAG_TIMEOUT_MS
 */

#include "comm/lora_frag.h"
#include "comm/comm_msg.h"
#include <string.h>

/* ================================================================
 * Fragmentation (émetteur)
 * ================================================================ */

int lora_frag_split(const uint8_t *data, size_t data_len,
                    uint8_t seq_id,
                    uint8_t packets[][LORA_FRAG_PACKET_MAX],
                    size_t *packet_lens,
                    uint8_t *packet_count)
{
    if (!data || !packets || !packet_lens || !packet_count) {
        return -1;
    }

    /* Calculer le nombre de fragments nécessaires */
    uint8_t count = (uint8_t)((data_len + LORA_FRAG_PAYLOAD_MAX - 1) / LORA_FRAG_PAYLOAD_MAX);

    /* Au minimum 1 fragment, même pour des données vides */
    if (count == 0) {
        count = 1;
    }

    /* Vérifier la limite de fragments */
    if (count > LORA_FRAG_MAX_FRAGMENTS) {
        return -1;
    }

    /* Découper les données en fragments */
    size_t offset = 0;
    for (uint8_t i = 0; i < count; i++) {
        /* Calculer la taille du payload de ce fragment */
        size_t payload_len = data_len - offset;
        if (payload_len > LORA_FRAG_PAYLOAD_MAX) {
            payload_len = LORA_FRAG_PAYLOAD_MAX;
        }

        /* Construire le header du fragment */
        packets[i][0] = COMM_MSG_LORA_FRAG;  /* Type */
        packets[i][1] = i;                    /* Index du fragment */
        packets[i][2] = count;                /* Nombre total */
        packets[i][3] = seq_id;               /* ID de séquence */

        /* Copier le payload */
        if (payload_len > 0) {
            memcpy(&packets[i][LORA_FRAG_HEADER_SIZE], &data[offset], payload_len);
        }

        packet_lens[i] = LORA_FRAG_HEADER_SIZE + payload_len;
        offset += payload_len;
    }

    *packet_count = count;
    return 0;
}

/* ================================================================
 * Réassemblage (récepteur)
 * ================================================================ */

void lora_frag_ctx_init(lora_frag_ctx_t *ctx)
{
    if (!ctx) return;
    memset(ctx, 0, sizeof(lora_frag_ctx_t));
}

bool lora_frag_receive(lora_frag_ctx_t *ctx,
                       uint8_t frag_index, uint8_t total,
                       uint8_t seq_id,
                       const uint8_t *payload, size_t payload_len,
                       uint64_t current_time)
{
    if (!ctx || !payload || total == 0 || total > LORA_FRAG_MAX_FRAGMENTS) {
        return false;
    }

    if (frag_index >= total) {
        return false;
    }

    if (payload_len > LORA_FRAG_PAYLOAD_MAX) {
        return false;
    }

    /*
     * Si le seq_id est différent de celui en cours,
     * abandonner l'ancien réassemblage et commencer un nouveau.
     */
    if (ctx->active && ctx->seq_id != seq_id) {
        lora_frag_ctx_init(ctx);
    }

    /* Démarrer un nouveau réassemblage si nécessaire */
    if (!ctx->active) {
        ctx->seq_id          = seq_id;
        ctx->total_fragments = total;
        ctx->received_mask   = 0;
        ctx->start_time      = current_time;
        ctx->active          = true;
    }

    /* Vérifier la cohérence du total avec la séquence en cours */
    if (total != ctx->total_fragments) {
        return false;
    }

    /* Ignorer les doublons (fragment déjà reçu) */
    if (ctx->received_mask & (1u << frag_index)) {
        /* Fragment déjà reçu, vérifier si complet */
        uint16_t complete_mask = (uint16_t)((1u << ctx->total_fragments) - 1);
        return (ctx->received_mask == complete_mask);
    }

    /* Copier le payload dans le buffer à la position du fragment */
    size_t buf_offset = (size_t)frag_index * LORA_FRAG_PAYLOAD_MAX;
    memcpy(&ctx->buffer[buf_offset], payload, payload_len);
    ctx->fragment_lens[frag_index] = payload_len;

    /* Marquer ce fragment comme reçu */
    ctx->received_mask |= (1u << frag_index);

    /* Vérifier si tous les fragments sont reçus */
    uint16_t complete_mask = (uint16_t)((1u << ctx->total_fragments) - 1);
    return (ctx->received_mask == complete_mask);
}

int lora_frag_get_result(const lora_frag_ctx_t *ctx,
                         uint8_t *out_buf, size_t out_buf_len,
                         size_t *out_len)
{
    if (!ctx || !out_buf || !out_len) {
        return -1;
    }

    /* Vérifier que le réassemblage est complet */
    if (!ctx->active) {
        return -1;
    }
    uint16_t complete_mask = (uint16_t)((1u << ctx->total_fragments) - 1);
    if (ctx->received_mask != complete_mask) {
        return -1;
    }

    /* Calculer la taille totale */
    size_t total_len = 0;
    for (uint8_t i = 0; i < ctx->total_fragments; i++) {
        total_len += ctx->fragment_lens[i];
    }

    if (out_buf_len < total_len) {
        return -1;
    }

    /* Concaténer les fragments dans l'ordre */
    size_t offset = 0;
    for (uint8_t i = 0; i < ctx->total_fragments; i++) {
        size_t buf_pos = (size_t)i * LORA_FRAG_PAYLOAD_MAX;
        memcpy(&out_buf[offset], &ctx->buffer[buf_pos], ctx->fragment_lens[i]);
        offset += ctx->fragment_lens[i];
    }

    *out_len = total_len;
    return 0;
}

void lora_frag_expire(lora_frag_ctx_t *ctx, uint64_t current_time)
{
    if (!ctx || !ctx->active) return;

    if (current_time - ctx->start_time >= LORA_FRAG_TIMEOUT_MS) {
        /* Timeout dépassé → abandonner le réassemblage */
        lora_frag_ctx_init(ctx);
    }
}
