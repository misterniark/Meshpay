/**
 * @file hal_lora_mock.c
 * @brief Mock LoRa — ring buffer loopback pour tests unitaires.
 *
 * Les paquets envoyés via send() sont empilés dans un ring buffer.
 * hal_lora_mock_pump() délivre le prochain paquet au callback RX.
 * Utile pour tester la couche comm/lora_sync sans matériel.
 */

#include "hal_lora_mock.h"
#include <string.h>

/** Nombre maximum de paquets en file d'attente */
#define MOCK_LORA_QUEUE_SIZE 16

/** Structure d'un paquet en file d'attente */
typedef struct {
    uint8_t data[HAL_LORA_MAX_PACKET_SIZE];
    size_t  len;
} mock_lora_packet_t;

/** Contexte interne du mock LoRa */
typedef struct {
    mock_lora_packet_t queue[MOCK_LORA_QUEUE_SIZE]; /* Ring buffer */
    uint32_t           head;           /* Index d'écriture */
    uint32_t           tail;           /* Index de lecture */
    uint32_t           count;          /* Nombre de paquets en attente */
    bool               initialized;    /* true après init */
    bool               rx_active;      /* true si start_rx appelé */
    hal_lora_rx_cb_t   rx_cb;          /* Callback de réception */
    void              *rx_user_ctx;    /* Contexte utilisateur du callback */
} mock_lora_ctx_t;

static mock_lora_ctx_t s_lora_ctx;

/* --- Implémentation de la vtable --- */

static hal_err_t mock_init(const hal_lora_config_t *config, void *ctx)
{
    (void)config;
    mock_lora_ctx_t *mc = (mock_lora_ctx_t *)ctx;
    mc->initialized = true;
    return HAL_OK;
}

/**
 * Envoyer un paquet : stocké dans le ring buffer pour relecture via pump().
 */
static hal_err_t mock_send(const uint8_t *data, size_t len, void *ctx)
{
    if (!data || len == 0 || len > HAL_LORA_MAX_PACKET_SIZE) {
        return HAL_ERR_INVALID;
    }

    mock_lora_ctx_t *mc = (mock_lora_ctx_t *)ctx;
    if (mc->count >= MOCK_LORA_QUEUE_SIZE) {
        return HAL_ERR_NO_MEM;
    }

    /* Copier le paquet dans le ring buffer */
    memcpy(mc->queue[mc->head].data, data, len);
    mc->queue[mc->head].len = len;
    mc->head = (mc->head + 1) % MOCK_LORA_QUEUE_SIZE;
    mc->count++;

    return HAL_OK;
}

static hal_err_t mock_set_rx_callback(hal_lora_rx_cb_t cb, void *user_ctx,
                                      void *ctx)
{
    mock_lora_ctx_t *mc = (mock_lora_ctx_t *)ctx;
    mc->rx_cb       = cb;
    mc->rx_user_ctx = user_ctx;
    return HAL_OK;
}

static hal_err_t mock_start_rx(void *ctx)
{
    mock_lora_ctx_t *mc = (mock_lora_ctx_t *)ctx;
    if (!mc->rx_cb) {
        return HAL_ERR_INVALID;
    }
    mc->rx_active = true;
    return HAL_OK;
}

static hal_err_t mock_sleep(void *ctx)
{
    mock_lora_ctx_t *mc = (mock_lora_ctx_t *)ctx;
    mc->rx_active = false;
    return HAL_OK;
}

/* --- API publique --- */

hal_err_t hal_lora_mock_create(hal_lora_t *lora)
{
    if (!lora) {
        return HAL_ERR_INVALID;
    }

    memset(&s_lora_ctx, 0, sizeof(s_lora_ctx));

    lora->init            = mock_init;
    lora->send            = mock_send;
    lora->set_rx_callback = mock_set_rx_callback;
    lora->start_rx        = mock_start_rx;
    lora->sleep           = mock_sleep;
    lora->ctx             = &s_lora_ctx;

    return HAL_OK;
}

void hal_lora_mock_reset(hal_lora_t *lora)
{
    if (!lora || !lora->ctx) return;
    memset(lora->ctx, 0, sizeof(mock_lora_ctx_t));
}

hal_err_t hal_lora_mock_pump(hal_lora_t *lora)
{
    if (!lora || !lora->ctx) return HAL_ERR_INVALID;

    mock_lora_ctx_t *mc = (mock_lora_ctx_t *)lora->ctx;

    if (mc->count == 0) {
        return HAL_ERR_NOT_FOUND;
    }
    if (!mc->rx_cb) {
        return HAL_ERR_INVALID;
    }

    /* Extraire le prochain paquet du ring buffer */
    mock_lora_packet_t *pkt = &mc->queue[mc->tail];
    mc->tail = (mc->tail + 1) % MOCK_LORA_QUEUE_SIZE;
    mc->count--;

    /* Délivrer au callback avec un RSSI fictif de -50 dBm */
    mc->rx_cb(pkt->data, pkt->len, -50, mc->rx_user_ctx);

    return HAL_OK;
}

uint32_t hal_lora_mock_pending_count(const hal_lora_t *lora)
{
    if (!lora || !lora->ctx) return 0;
    const mock_lora_ctx_t *mc = (const mock_lora_ctx_t *)lora->ctx;
    return mc->count;
}
