/**
 * @file test_comm_msg_dag_sync.c
 * @brief Tests des messages LoRa de synchronisation DAG.
 */

#include "unity.h"
#include "comm/comm_msg.h"
#include "crypto/crypto_init.h"
#include "crypto/crypto_keys.h"
#include "crypto/crypto_sign.h"
#include <string.h>

static void fill_pubkey(public_key_t *key, uint8_t pattern)
{
    memset(key->bytes, pattern, CRYPTO_PUBLIC_KEY_SIZE);
}

static void fill_signature(signature_t *sig, uint8_t pattern)
{
    memset(sig->bytes, pattern, CRYPTO_SIGNATURE_SIZE);
}

static void fill_hash(hash_t *h, uint8_t pattern)
{
    memset(h->bytes, pattern, CRYPTO_HASH_SIZE);
}

static void write_u16_be_test(uint8_t *dst, uint16_t val)
{
    dst[0] = (uint8_t)(val >> 8);
    dst[1] = (uint8_t)(val & 0xFF);
}

static void write_u64_be_test(uint8_t *dst, uint64_t val)
{
    for (int i = 7; i >= 0; --i) {
        dst[7 - i] = (uint8_t)((val >> (i * 8)) & 0xFF);
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

    TEST_ASSERT_EQUAL(ESP_OK, crypto_sign(signed_buf, len,
                                          keypair, &msg->signature));
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

    TEST_ASSERT_EQUAL(ESP_OK, crypto_sign(signed_buf, len,
                                          keypair, &msg->signature));
}

TEST_CASE("dag_summary_roundtrip_and_verify", "[comm_msg]")
{
    TEST_ASSERT_EQUAL(ESP_OK, crypto_init());

    keypair_t kp;
    TEST_ASSERT_EQUAL(ESP_OK, crypto_generate_keypair(&kp));

    comm_msg_dag_summary_t msg = {
        .checkpoint_timestamp = 1000,
        .last_tx_timestamp = 2500,
        .tx_count_window = 42,
        .tip_count = COMM_MSG_DAG_SUMMARY_MAX_TIPS,
    };
    memcpy(&msg.node_key, &kp.public_key, sizeof(public_key_t));
    fill_hash(&msg.tips[0], 0xA1);
    fill_hash(&msg.tips[1], 0xB2);
    sign_summary(&msg, &kp);

    uint8_t buf[COMM_MSG_LORA_MAX];
    size_t out_len = 0;
    TEST_ASSERT_EQUAL(0, comm_msg_pack_dag_summary(buf, sizeof(buf),
                                                   &msg, &out_len));
    TEST_ASSERT_EQUAL(COMM_MSG_DAG_SUMMARY_MAX_SIZE, out_len);

    comm_msg_dag_summary_t decoded;
    TEST_ASSERT_EQUAL(0, comm_msg_unpack_dag_summary(buf, out_len, &decoded));
    TEST_ASSERT_EQUAL_MEMORY(msg.node_key.bytes, decoded.node_key.bytes,
                             CRYPTO_PUBLIC_KEY_SIZE);
    TEST_ASSERT_EQUAL_MEMORY(msg.signature.bytes, decoded.signature.bytes,
                             CRYPTO_SIGNATURE_SIZE);
    TEST_ASSERT_EQUAL_UINT64(msg.checkpoint_timestamp,
                             decoded.checkpoint_timestamp);
    TEST_ASSERT_EQUAL_UINT64(msg.last_tx_timestamp,
                             decoded.last_tx_timestamp);
    TEST_ASSERT_EQUAL_UINT16(msg.tx_count_window, decoded.tx_count_window);
    TEST_ASSERT_EQUAL_UINT8(msg.tip_count, decoded.tip_count);
    TEST_ASSERT_EQUAL_MEMORY(msg.tips[0].bytes, decoded.tips[0].bytes,
                             CRYPTO_HASH_SIZE);
    TEST_ASSERT_EQUAL_MEMORY(msg.tips[1].bytes, decoded.tips[1].bytes,
                             CRYPTO_HASH_SIZE);
    TEST_ASSERT_EQUAL(0, comm_msg_verify_dag_summary(&decoded));
}

TEST_CASE("dag_summary_rejects_tip_overflow", "[comm_msg]")
{
    comm_msg_dag_summary_t msg = {
        .tip_count = COMM_MSG_DAG_SUMMARY_MAX_TIPS + 1,
    };
    uint8_t buf[COMM_MSG_LORA_MAX];
    size_t out_len = 0;

    TEST_ASSERT_EQUAL(-1, comm_msg_pack_dag_summary(buf, sizeof(buf),
                                                    &msg, &out_len));
    TEST_ASSERT_EQUAL(-1, comm_msg_verify_dag_summary(&msg));
}

TEST_CASE("dag_summary_verify_detects_tamper", "[comm_msg]")
{
    TEST_ASSERT_EQUAL(ESP_OK, crypto_init());

    keypair_t kp;
    TEST_ASSERT_EQUAL(ESP_OK, crypto_generate_keypair(&kp));

    comm_msg_dag_summary_t msg = {
        .checkpoint_timestamp = 11,
        .last_tx_timestamp = 22,
        .tx_count_window = 3,
        .tip_count = 1,
    };
    memcpy(&msg.node_key, &kp.public_key, sizeof(public_key_t));
    fill_hash(&msg.tips[0], 0x5A);
    sign_summary(&msg, &kp);

    TEST_ASSERT_EQUAL(0, comm_msg_verify_dag_summary(&msg));
    msg.last_tx_timestamp++;
    TEST_ASSERT_EQUAL(-1, comm_msg_verify_dag_summary(&msg));
}

TEST_CASE("dag_request_roundtrip_and_verify", "[comm_msg]")
{
    TEST_ASSERT_EQUAL(ESP_OK, crypto_init());

    keypair_t requester;
    TEST_ASSERT_EQUAL(ESP_OK, crypto_generate_keypair(&requester));

    comm_msg_dag_request_t msg = {
        .since_timestamp = 123456,
        .max_count = 12,
    };
    memcpy(&msg.requester_key, &requester.public_key, sizeof(public_key_t));
    fill_pubkey(&msg.target_key, 0xC4);
    sign_request(&msg, &requester);

    uint8_t buf[COMM_MSG_LORA_MAX];
    size_t out_len = 0;
    TEST_ASSERT_EQUAL(0, comm_msg_pack_dag_request(buf, sizeof(buf),
                                                   &msg, &out_len));
    TEST_ASSERT_EQUAL(COMM_MSG_DAG_REQUEST_SIZE, out_len);

    comm_msg_dag_request_t decoded;
    TEST_ASSERT_EQUAL(0, comm_msg_unpack_dag_request(buf, out_len, &decoded));
    TEST_ASSERT_EQUAL_MEMORY(msg.requester_key.bytes,
                             decoded.requester_key.bytes,
                             CRYPTO_PUBLIC_KEY_SIZE);
    TEST_ASSERT_EQUAL_MEMORY(msg.target_key.bytes, decoded.target_key.bytes,
                             CRYPTO_PUBLIC_KEY_SIZE);
    TEST_ASSERT_EQUAL_MEMORY(msg.signature.bytes, decoded.signature.bytes,
                             CRYPTO_SIGNATURE_SIZE);
    TEST_ASSERT_EQUAL_UINT64(msg.since_timestamp, decoded.since_timestamp);
    TEST_ASSERT_EQUAL_UINT8(msg.max_count, decoded.max_count);
    TEST_ASSERT_EQUAL(0, comm_msg_verify_dag_request(&decoded));
}

TEST_CASE("dag_request_rejects_zero_max_count", "[comm_msg]")
{
    comm_msg_dag_request_t msg = { .max_count = 0 };
    uint8_t buf[COMM_MSG_DAG_REQUEST_SIZE] = {0};
    size_t out_len = 0;

    TEST_ASSERT_EQUAL(-1, comm_msg_pack_dag_request(buf, sizeof(buf),
                                                    &msg, &out_len));

    buf[0] = COMM_MSG_LORA_DAG_REQUEST;
    TEST_ASSERT_EQUAL(-1, comm_msg_unpack_dag_request(buf, sizeof(buf),
                                                      &msg));
    TEST_ASSERT_EQUAL(-1, comm_msg_verify_dag_request(&msg));
}

TEST_CASE("dag_request_verify_detects_tamper", "[comm_msg]")
{
    TEST_ASSERT_EQUAL(ESP_OK, crypto_init());

    keypair_t requester;
    TEST_ASSERT_EQUAL(ESP_OK, crypto_generate_keypair(&requester));

    comm_msg_dag_request_t msg = {
        .since_timestamp = 77,
        .max_count = 4,
    };
    memcpy(&msg.requester_key, &requester.public_key, sizeof(public_key_t));
    fill_pubkey(&msg.target_key, 0xD5);
    sign_request(&msg, &requester);

    TEST_ASSERT_EQUAL(0, comm_msg_verify_dag_request(&msg));
    msg.target_key.bytes[0] ^= 0x7F;
    TEST_ASSERT_EQUAL(-1, comm_msg_verify_dag_request(&msg));
}

TEST_CASE("dag_attest_batch_roundtrip", "[comm_msg]")
{
    comm_msg_dag_attest_batch_t msg = {
        .count = COMM_MSG_DAG_ATTEST_BATCH_MAX,
    };
    fill_pubkey(&msg.attester_key, 0x11);
    fill_signature(&msg.signatures[0], 0x22);
    fill_signature(&msg.signatures[1], 0x33);
    fill_hash(&msg.tx_ids[0], 0x44);
    fill_hash(&msg.tx_ids[1], 0x55);

    uint8_t buf[COMM_MSG_LORA_MAX];
    size_t out_len = 0;
    TEST_ASSERT_EQUAL(0, comm_msg_pack_dag_attest_batch(buf, sizeof(buf),
                                                        &msg, &out_len));
    TEST_ASSERT_EQUAL(COMM_MSG_DAG_ATTEST_BATCH_MAX_SIZE, out_len);

    comm_msg_dag_attest_batch_t decoded;
    TEST_ASSERT_EQUAL(0, comm_msg_unpack_dag_attest_batch(buf, out_len,
                                                          &decoded));
    TEST_ASSERT_EQUAL_UINT8(msg.count, decoded.count);
    TEST_ASSERT_EQUAL_MEMORY(msg.attester_key.bytes,
                             decoded.attester_key.bytes,
                             CRYPTO_PUBLIC_KEY_SIZE);
    for (uint8_t i = 0; i < msg.count; i++) {
        TEST_ASSERT_EQUAL_MEMORY(msg.signatures[i].bytes,
                                 decoded.signatures[i].bytes,
                                 CRYPTO_SIGNATURE_SIZE);
        TEST_ASSERT_EQUAL_MEMORY(msg.tx_ids[i].bytes, decoded.tx_ids[i].bytes,
                                 CRYPTO_HASH_SIZE);
    }
}

TEST_CASE("dag_attest_batch_rejects_zero_or_overflow_count", "[comm_msg]")
{
    comm_msg_dag_attest_batch_t msg = { .count = 0 };
    uint8_t buf[COMM_MSG_LORA_MAX];
    size_t out_len = 0;

    TEST_ASSERT_EQUAL(-1, comm_msg_pack_dag_attest_batch(buf, sizeof(buf),
                                                         &msg, &out_len));

    msg.count = COMM_MSG_DAG_ATTEST_BATCH_MAX + 1;
    TEST_ASSERT_EQUAL(-1, comm_msg_pack_dag_attest_batch(buf, sizeof(buf),
                                                         &msg, &out_len));

    memset(buf, 0, sizeof(buf));
    buf[0] = COMM_MSG_LORA_DAG_ATTEST_BATCH;
    buf[1 + CRYPTO_PUBLIC_KEY_SIZE] = 0;
    TEST_ASSERT_EQUAL(-1, comm_msg_unpack_dag_attest_batch(
                               buf, COMM_MSG_DAG_ATTEST_BATCH_MIN_SIZE, &msg));
}
