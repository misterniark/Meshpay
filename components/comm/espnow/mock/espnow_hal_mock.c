/**
 * @file espnow_hal_mock.c
 * @brief Mock ESP-NOW — ring buffer d'envoi + injection de réception.
 *
 * Les paquets envoyés via send()/broadcast() sont stockés dans un
 * historique. On peut les inspecter avec get_last_sent().
 *
 * inject() appelle directement le callback RX pour simuler la
 * réception d'un paquet d'un autre device.
 */

#include "espnow_hal_mock.h"
#include "comm/comm_msg.h"  /* COMM_MSG_ESPNOW_MAX */
#include <string.h>

/** Nombre max de paquets stockés dans l'historique d'envoi */
#define MOCK_SEND_HISTORY_SIZE 16

/** Paquet stocké dans l'historique */
typedef struct {
    uint8_t dest_mac[6];
    uint8_t data[COMM_MSG_ESPNOW_MAX];
    size_t  len;
    bool    used;
} mock_sent_packet_t;

/** Contexte interne du mock ESP-NOW */
typedef struct {
    bool              initialized;
    espnow_rx_cb_t    rx_cb;
    void             *rx_user_ctx;
    mock_sent_packet_t history[MOCK_SEND_HISTORY_SIZE];
    uint32_t          send_count;    /* Nombre total de paquets envoyés */
    uint32_t          history_head;  /* Index circulaire d'écriture */
} mock_espnow_ctx_t;

static mock_espnow_ctx_t s_mock_ctx;

/* --- Implémentation vtable --- */

static hal_err_t mock_init(void *ctx)
{
    mock_espnow_ctx_t *mc = (mock_espnow_ctx_t *)ctx;
    mc->initialized = true;
    return HAL_OK;
}

static hal_err_t mock_deinit(void *ctx)
{
    mock_espnow_ctx_t *mc = (mock_espnow_ctx_t *)ctx;
    mc->initialized = false;
    return HAL_OK;
}

/**
 * Stocker un paquet dans l'historique d'envoi.
 */
static hal_err_t mock_send(const uint8_t *dest_mac, const uint8_t *data,
                            size_t len, void *ctx)
{
    /* [F-EN-001] Aligné sur COMM_MSG_ESPNOW_MAX (321) — V2 ESP-NOW. */
    if (!dest_mac || !data || len == 0 || len > COMM_MSG_ESPNOW_MAX) {
        return HAL_ERR_INVALID;
    }

    mock_espnow_ctx_t *mc = (mock_espnow_ctx_t *)ctx;

    /* Écrire dans l'historique circulaire */
    uint32_t idx = mc->history_head % MOCK_SEND_HISTORY_SIZE;
    memcpy(mc->history[idx].dest_mac, dest_mac, 6);
    memcpy(mc->history[idx].data, data, len);
    mc->history[idx].len  = len;
    mc->history[idx].used = true;

    mc->history_head++;
    mc->send_count++;

    return HAL_OK;
}

static hal_err_t mock_broadcast(const uint8_t *data, size_t len, void *ctx)
{
    uint8_t broadcast_addr[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    return mock_send(broadcast_addr, data, len, ctx);
}

static hal_err_t mock_set_rx_callback(espnow_rx_cb_t cb, void *user_ctx,
                                       void *ctx)
{
    mock_espnow_ctx_t *mc = (mock_espnow_ctx_t *)ctx;
    mc->rx_cb       = cb;
    mc->rx_user_ctx = user_ctx;
    return HAL_OK;
}

/* --- API publique du mock --- */

hal_err_t espnow_hal_mock_create(espnow_hal_t *hal)
{
    if (!hal) return HAL_ERR_INVALID;

    memset(&s_mock_ctx, 0, sizeof(s_mock_ctx));

    hal->init           = mock_init;
    hal->deinit         = mock_deinit;
    hal->send           = mock_send;
    hal->broadcast      = mock_broadcast;
    hal->set_rx_callback = mock_set_rx_callback;
    hal->ctx            = &s_mock_ctx;

    return HAL_OK;
}

void espnow_hal_mock_reset(espnow_hal_t *hal)
{
    if (!hal || !hal->ctx) return;
    memset(hal->ctx, 0, sizeof(mock_espnow_ctx_t));
}

hal_err_t espnow_hal_mock_inject(espnow_hal_t *hal,
                                  const uint8_t *src_mac,
                                  const uint8_t *data, size_t len)
{
    if (!hal || !hal->ctx || !src_mac || !data) {
        return HAL_ERR_INVALID;
    }

    mock_espnow_ctx_t *mc = (mock_espnow_ctx_t *)hal->ctx;
    if (!mc->rx_cb) {
        return HAL_ERR_INVALID;
    }

    /* Appeler directement le callback RX */
    mc->rx_cb(src_mac, data, len, mc->rx_user_ctx);
    return HAL_OK;
}

hal_err_t espnow_hal_mock_get_last_sent(const espnow_hal_t *hal,
                                         uint8_t *dest_mac,
                                         uint8_t *data, size_t *len)
{
    if (!hal || !hal->ctx || !dest_mac || !data || !len) {
        return HAL_ERR_INVALID;
    }

    const mock_espnow_ctx_t *mc = (const mock_espnow_ctx_t *)hal->ctx;
    if (mc->send_count == 0) {
        return HAL_ERR_NOT_FOUND;
    }

    /* Récupérer le dernier paquet écrit */
    uint32_t idx = (mc->history_head - 1) % MOCK_SEND_HISTORY_SIZE;
    const mock_sent_packet_t *pkt = &mc->history[idx];

    if (*len < pkt->len) {
        *len = pkt->len;
        return HAL_ERR_NO_MEM;
    }

    memcpy(dest_mac, pkt->dest_mac, 6);
    memcpy(data, pkt->data, pkt->len);
    *len = pkt->len;
    return HAL_OK;
}

uint32_t espnow_hal_mock_sent_count(const espnow_hal_t *hal)
{
    if (!hal || !hal->ctx) return 0;
    const mock_espnow_ctx_t *mc = (const mock_espnow_ctx_t *)hal->ctx;
    return mc->send_count;
}
