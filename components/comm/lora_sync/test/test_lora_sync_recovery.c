/**
 * @file test_lora_sync_recovery.c
 * @brief Tests de reprise/pagination de la synchronisation DAG sur LoRa.
 */

#include "unity.h"

#include "comm/comm_msg.h"
#include "comm/lora_sync.h"
#include "crypto/crypto_init.h"
#include "crypto/crypto_keys.h"
#include "crypto/crypto_sign.h"
#include "transaction/tx_serialize.h"

#include <string.h>

#define CAPTURE_MAX 16

typedef struct {
    uint8_t packets[CAPTURE_MAX][COMM_MSG_LORA_MAX];
    size_t  lens[CAPTURE_MAX];
    uint32_t count;
} capture_lora_t;

typedef struct {
    transaction_t txs[8];
    uint32_t tx_count;
    uint64_t since_seen;
    uint32_t max_seen;
    lora_dag_summary_t summary;
} recovery_ctx_t;

static hal_err_t capture_send(const uint8_t *data, size_t len, void *ctx)
{
    capture_lora_t *cap = (capture_lora_t *)ctx;
    if (!cap || !data || len > COMM_MSG_LORA_MAX || cap->count >= CAPTURE_MAX) {
        return HAL_ERR_INVALID;
    }
    memcpy(cap->packets[cap->count], data, len);
    cap->lens[cap->count] = len;
    cap->count++;
    return HAL_OK;
}

static uint32_t collect_test_txs(uint64_t since_ts,
                                 transaction_t *out_buf,
                                 uint32_t max_count,
                                 uint64_t *out_newest_ts,
                                 void *ctx)
{
    recovery_ctx_t *rc = (recovery_ctx_t *)ctx;
    rc->since_seen = since_ts;
    rc->max_seen = max_count;

    uint32_t written = 0;
    uint64_t newest = since_ts;
    for (uint32_t i = 0; i < rc->tx_count && written < max_count; i++) {
        if (rc->txs[i].timestamp <= since_ts) {
            continue;
        }
        out_buf[written++] = rc->txs[i];
        if (rc->txs[i].timestamp > newest) {
            newest = rc->txs[i].timestamp;
        }
    }
    *out_newest_ts = newest;
    return written;
}

static bool get_test_summary(lora_dag_summary_t *out_summary, void *ctx)
{
    recovery_ctx_t *rc = (recovery_ctx_t *)ctx;
    *out_summary = rc->summary;
    return true;
}

static void write_u16_be_test(uint8_t *dst, uint16_t val)
{
    dst[0] = (uint8_t)(val >> 8);
    dst[1] = (uint8_t)val;
}

static void write_u64_be_test(uint8_t *dst, uint64_t val)
{
    for (int i = 7; i >= 0; i--) {
        dst[7 - i] = (uint8_t)(val >> (i * 8));
    }
}

static void sign_summary(comm_msg_dag_summary_t *msg,
                         const keypair_t *keypair)
{
    uint8_t signed_buf[8 + 8 + 2 + 1 +
                       (COMM_MSG_DAG_SUMMARY_MAX_TIPS * CRYPTO_HASH_SIZE)];
    size_t len = 0;
    write_u64_be_test(&signed_buf[len], msg->checkpoint_timestamp);
    len += 8;
    write_u64_be_test(&signed_buf[len], msg->last_tx_timestamp);
    len += 8;
    write_u16_be_test(&signed_buf[len], msg->tx_count_window);
    len += 2;
    signed_buf[len++] = msg->tip_count;
    for (uint8_t i = 0; i < msg->tip_count; i++) {
        memcpy(&signed_buf[len], msg->tips[i].bytes, CRYPTO_HASH_SIZE);
        len += CRYPTO_HASH_SIZE;
    }
    TEST_ASSERT_EQUAL(ESP_OK, crypto_sign(signed_buf, len, keypair,
                                          &msg->signature));
}

static void sign_request(comm_msg_dag_request_t *msg,
                         const keypair_t *keypair)
{
    uint8_t signed_buf[CRYPTO_PUBLIC_KEY_SIZE + 8 + 1];
    size_t len = 0;
    memcpy(&signed_buf[len], msg->target_key.bytes, CRYPTO_PUBLIC_KEY_SIZE);
    len += CRYPTO_PUBLIC_KEY_SIZE;
    write_u64_be_test(&signed_buf[len], msg->since_timestamp);
    len += 8;
    signed_buf[len++] = msg->max_count;
    TEST_ASSERT_EQUAL(ESP_OK, crypto_sign(signed_buf, len, keypair,
                                          &msg->signature));
}

static void make_test_tx(transaction_t *tx, uint64_t ts, uint8_t id_byte)
{
    memset(tx, 0, sizeof(*tx));
    tx->type = TX_TYPE_TRANSFER;
    tx->status = TX_STATUS_CONFIRMED;
    tx->parent_count = 1;
    tx->timestamp = ts;
    memset(tx->id.bytes, id_byte, sizeof(tx->id.bytes));
}

static void make_config(lora_sync_config_t *cfg,
                        hal_lora_t *hal,
                        capture_lora_t *cap,
                        recovery_ctx_t *rc,
                        const keypair_t *own)
{
    memset(cfg, 0, sizeof(*cfg));
    memset(hal, 0, sizeof(*hal));
    hal->send = capture_send;
    hal->ctx = cap;
    cfg->lora = hal;
    cfg->collect_confirmed_txs = collect_test_txs;
    cfg->get_dag_summary = get_test_summary;
    cfg->collect_ctx = rc;
    cfg->own_pubkey = &own->public_key;
    cfg->own_keypair = own;
}

TEST_CASE("lora_summary_newer_requests_resume_with_overlap", "[lora_sync]")
{
    TEST_ASSERT_EQUAL(ESP_OK, crypto_init());

    keypair_t own;
    keypair_t peer;
    TEST_ASSERT_EQUAL(ESP_OK, crypto_generate_keypair(&own));
    TEST_ASSERT_EQUAL(ESP_OK, crypto_generate_keypair(&peer));

    capture_lora_t cap = {0};
    recovery_ctx_t rc = {0};
    rc.summary.last_tx_timestamp = 1000;

    hal_lora_t hal;
    lora_sync_config_t cfg;
    make_config(&cfg, &hal, &cap, &rc, &own);

    comm_msg_dag_summary_t summary = {
        .node_key = peer.public_key,
        .checkpoint_timestamp = 500,
        .last_tx_timestamp = 2000,
        .tx_count_window = 4,
        .tip_count = 0,
    };
    sign_summary(&summary, &peer);

    uint8_t buf[COMM_MSG_DAG_SUMMARY_MAX_SIZE];
    size_t len = 0;
    TEST_ASSERT_EQUAL(0, comm_msg_pack_dag_summary(buf, sizeof(buf),
                                                   &summary, &len));

    lora_sync_handle_rx(&cfg, buf, len);

    TEST_ASSERT_EQUAL_UINT32(1, cap.count);
    TEST_ASSERT_EQUAL_UINT8(COMM_MSG_LORA_DAG_REQUEST, cap.packets[0][0]);

    comm_msg_dag_request_t req;
    TEST_ASSERT_EQUAL(0, comm_msg_unpack_dag_request(cap.packets[0],
                                                     cap.lens[0], &req));
    TEST_ASSERT_EQUAL_MEMORY(peer.public_key.bytes, req.target_key.bytes,
                             CRYPTO_PUBLIC_KEY_SIZE);
    TEST_ASSERT_EQUAL_UINT64(999, req.since_timestamp);
    TEST_ASSERT_EQUAL_UINT8(8, req.max_count);
    TEST_ASSERT_EQUAL(0, comm_msg_verify_dag_request(&req));

    lora_sync_handle_rx(&cfg, buf, len);
    TEST_ASSERT_EQUAL_UINT32(1, cap.count);
}

TEST_CASE("lora_request_response_is_sorted_capped_and_acknowledged", "[lora_sync]")
{
    TEST_ASSERT_EQUAL(ESP_OK, crypto_init());

    keypair_t own;
    keypair_t peer;
    TEST_ASSERT_EQUAL(ESP_OK, crypto_generate_keypair(&own));
    TEST_ASSERT_EQUAL(ESP_OK, crypto_generate_keypair(&peer));

    capture_lora_t cap = {0};
    recovery_ctx_t rc = {0};
    rc.tx_count = 3;
    make_test_tx(&rc.txs[0], 300, 0x30);
    make_test_tx(&rc.txs[1], 100, 0x10);
    make_test_tx(&rc.txs[2], 200, 0x20);
    rc.summary.last_tx_timestamp = 300;
    rc.summary.tx_count_window = 3;

    hal_lora_t hal;
    lora_sync_config_t cfg;
    make_config(&cfg, &hal, &cap, &rc, &own);

    comm_msg_dag_request_t req = {
        .requester_key = peer.public_key,
        .target_key = own.public_key,
        .since_timestamp = 0,
        .max_count = 20,
    };
    sign_request(&req, &peer);

    uint8_t req_buf[COMM_MSG_DAG_REQUEST_SIZE];
    size_t req_len = 0;
    TEST_ASSERT_EQUAL(0, comm_msg_pack_dag_request(req_buf, sizeof(req_buf),
                                                   &req, &req_len));

    lora_sync_handle_rx(&cfg, req_buf, req_len);

    TEST_ASSERT_EQUAL_UINT32(8, rc.max_seen);
    TEST_ASSERT_EQUAL_UINT64(0, rc.since_seen);
    TEST_ASSERT_EQUAL_UINT32(4, cap.count);
    TEST_ASSERT_EQUAL_UINT8(COMM_MSG_LORA_TX, cap.packets[0][0]);
    TEST_ASSERT_EQUAL_UINT8(COMM_MSG_LORA_TX, cap.packets[1][0]);
    TEST_ASSERT_EQUAL_UINT8(COMM_MSG_LORA_TX, cap.packets[2][0]);
    TEST_ASSERT_EQUAL_UINT8(COMM_MSG_LORA_DAG_SUMMARY, cap.packets[3][0]);

    transaction_t decoded;
    TEST_ASSERT_EQUAL(ESP_OK, tx_deserialize(&cap.packets[0][1],
                                             cap.lens[0] - 1, &decoded));
    TEST_ASSERT_EQUAL_UINT64(100, decoded.timestamp);
    TEST_ASSERT_EQUAL(ESP_OK, tx_deserialize(&cap.packets[1][1],
                                             cap.lens[1] - 1, &decoded));
    TEST_ASSERT_EQUAL_UINT64(200, decoded.timestamp);
    TEST_ASSERT_EQUAL(ESP_OK, tx_deserialize(&cap.packets[2][1],
                                             cap.lens[2] - 1, &decoded));
    TEST_ASSERT_EQUAL_UINT64(300, decoded.timestamp);

    comm_msg_dag_summary_t ack;
    TEST_ASSERT_EQUAL(0, comm_msg_unpack_dag_summary(cap.packets[3],
                                                     cap.lens[3], &ack));
    TEST_ASSERT_EQUAL_UINT64(300, ack.last_tx_timestamp);
    TEST_ASSERT_EQUAL(0, comm_msg_verify_dag_summary(&ack));
}
