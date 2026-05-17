/**
 * @file test_tx_lifecycle_chaos.c
 * @brief Tests du cycle de vie TX sous pertes, doublons et ordre inverse.
 */

#include "unity.h"

#include "crypto/crypto_init.h"
#include "crypto/crypto_keys.h"
#include "crypto/crypto_sign.h"
#include "dag/dag.h"
#include "transaction/tx_create.h"
#include "tx_lifecycle/tx_lifecycle.h"
#include "wallet/wallet.h"
#include "wallet/wallet_lock.h"

#include <string.h>

static uint64_t s_mock_time_ms;

static uint64_t mock_get_time(void)
{
    return s_mock_time_ms;
}

static void make_hash(hash_t *h, uint8_t fill)
{
    memset(h->bytes, fill, CRYPTO_HASH_SIZE);
}

static void make_transfer(transaction_t *tx,
                          keypair_t *from,
                          keypair_t *to,
                          uint32_t seq,
                          uint64_t timestamp)
{
    hash_t parent;
    make_hash(&parent, (uint8_t)(0x40 + seq));
    TEST_ASSERT_EQUAL(ESP_OK,
        tx_create_transfer(tx, from, &to->public_key, 10, 1, 0,
                           seq, &parent, 1, timestamp));
}

static void make_attestation(const keypair_t *attester,
                             const hash_t *tx_id,
                             comm_msg_attestation_t *out)
{
    memset(out, 0, sizeof(*out));
    memcpy(&out->attester_key, &attester->public_key, sizeof(public_key_t));
    memcpy(&out->tx_id, tx_id, sizeof(hash_t));
    TEST_ASSERT_EQUAL(ESP_OK,
        crypto_sign(tx_id->bytes, CRYPTO_HASH_SIZE, attester,
                    &out->signature));
}

static const transaction_t *must_get(const dag_t *dag, const hash_t *tx_id)
{
    const transaction_t *tx = dag_get_by_id(dag, tx_id);
    TEST_ASSERT_NOT_NULL(tx);
    return tx;
}

TEST_CASE("tx_lifecycle_chaos_attestation_before_tx_then_replay", "[tx_lifecycle][chaos]")
{
    TEST_ASSERT_EQUAL(ESP_OK, crypto_init());

    keypair_t alice;
    keypair_t bob;
    TEST_ASSERT_EQUAL(ESP_OK, crypto_generate_keypair(&alice));
    TEST_ASSERT_EQUAL(ESP_OK, crypto_generate_keypair(&bob));

    transaction_t tx;
    make_transfer(&tx, &alice, &bob, 1, 1000);

    comm_msg_attestation_t att;
    make_attestation(&bob, &tx.id, &att);

    dag_t observer;
    TEST_ASSERT_EQUAL(ESP_OK, dag_init(&observer));

    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND,
        tx_lifecycle_confirm_by_attestation(
            &observer, &att, TX_LIFECYCLE_CONFIRM_ATTESTATION));

    TEST_ASSERT_EQUAL(ESP_OK, dag_insert(&observer, &tx));
    TEST_ASSERT_EQUAL(TX_STATUS_LOCKED,
                      must_get(&observer, &tx.id)->status);

    uint32_t applied = 0;
    TEST_ASSERT_EQUAL(ESP_OK,
        tx_lifecycle_replay_attestations(&observer, &att, 1, &applied));
    TEST_ASSERT_EQUAL_UINT32(1, applied);
    TEST_ASSERT_EQUAL(TX_STATUS_CONFIRMED,
                      must_get(&observer, &tx.id)->status);

    applied = 0;
    TEST_ASSERT_EQUAL(ESP_OK,
        tx_lifecycle_replay_attestations(&observer, &att, 1, &applied));
    TEST_ASSERT_EQUAL_UINT32(0, applied);
    TEST_ASSERT_EQUAL(TX_STATUS_CONFIRMED,
                      must_get(&observer, &tx.id)->status);
}

TEST_CASE("tx_lifecycle_chaos_duplicate_ack_and_attestation_are_idempotent", "[tx_lifecycle][chaos]")
{
    TEST_ASSERT_EQUAL(ESP_OK, crypto_init());
    s_mock_time_ms = 100;

    keypair_t alice;
    keypair_t bob;
    TEST_ASSERT_EQUAL(ESP_OK, crypto_generate_keypair(&alice));
    TEST_ASSERT_EQUAL(ESP_OK, crypto_generate_keypair(&bob));

    dag_t sender_dag;
    TEST_ASSERT_EQUAL(ESP_OK, dag_init(&sender_dag));

    transaction_t tx;
    make_transfer(&tx, &alice, &bob, 2, 2000);
    TEST_ASSERT_EQUAL(ESP_OK, dag_insert(&sender_dag, &tx));

    wallet_t wallet;
    TEST_ASSERT_EQUAL(ESP_OK,
        wallet_init(&wallet, &alice.public_key, &sender_dag, mock_get_time));
    lock_table_t locks;
    TEST_ASSERT_EQUAL(ESP_OK, lock_table_init(&locks, &wallet));
    TEST_ASSERT_EQUAL(ESP_OK, lock_table_lock(&locks, &tx.id, tx.amount));

    TEST_ASSERT_EQUAL(ESP_OK,
        tx_lifecycle_confirm_by_ack(&sender_dag, &locks, &tx.id,
                                    &bob.public_key));
    TEST_ASSERT_EQUAL(TX_STATUS_CONFIRMED,
                      must_get(&sender_dag, &tx.id)->status);

    TEST_ASSERT_EQUAL(ESP_OK,
        tx_lifecycle_confirm_by_ack(&sender_dag, &locks, &tx.id,
                                    &bob.public_key));

    comm_msg_attestation_t att;
    make_attestation(&bob, &tx.id, &att);
    TEST_ASSERT_EQUAL(ESP_OK,
        tx_lifecycle_confirm_by_attestation(
            &sender_dag, &att, TX_LIFECYCLE_CONFIRM_ATTESTATION));
    TEST_ASSERT_EQUAL(TX_STATUS_CONFIRMED,
                      must_get(&sender_dag, &tx.id)->status);
}

TEST_CASE("tx_lifecycle_chaos_forged_or_late_attestation_cannot_resurrect", "[tx_lifecycle][chaos]")
{
    TEST_ASSERT_EQUAL(ESP_OK, crypto_init());

    keypair_t alice;
    keypair_t bob;
    keypair_t mallory;
    TEST_ASSERT_EQUAL(ESP_OK, crypto_generate_keypair(&alice));
    TEST_ASSERT_EQUAL(ESP_OK, crypto_generate_keypair(&bob));
    TEST_ASSERT_EQUAL(ESP_OK, crypto_generate_keypair(&mallory));

    dag_t dag;
    TEST_ASSERT_EQUAL(ESP_OK, dag_init(&dag));

    transaction_t tx;
    make_transfer(&tx, &alice, &bob, 3, 3000);
    TEST_ASSERT_EQUAL(ESP_OK, dag_insert(&dag, &tx));

    comm_msg_attestation_t forged;
    make_attestation(&mallory, &tx.id, &forged);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE,
        tx_lifecycle_confirm_by_attestation(
            &dag, &forged, TX_LIFECYCLE_CONFIRM_ATTESTATION));
    TEST_ASSERT_EQUAL(TX_STATUS_LOCKED, must_get(&dag, &tx.id)->status);

    TEST_ASSERT_EQUAL(ESP_OK,
        tx_lifecycle_cancel(&dag, NULL, &tx.id,
                            TX_LIFECYCLE_CANCEL_TIMEOUT));
    TEST_ASSERT_EQUAL(TX_STATUS_CANCELLED, must_get(&dag, &tx.id)->status);

    comm_msg_attestation_t late_valid;
    make_attestation(&bob, &tx.id, &late_valid);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE,
        tx_lifecycle_confirm_by_attestation(
            &dag, &late_valid, TX_LIFECYCLE_CONFIRM_ATTESTATION));
    TEST_ASSERT_EQUAL(TX_STATUS_CANCELLED, must_get(&dag, &tx.id)->status);
}

TEST_CASE("tx_lifecycle_chaos_wrong_ack_signer_is_rejected", "[tx_lifecycle][chaos]")
{
    TEST_ASSERT_EQUAL(ESP_OK, crypto_init());

    keypair_t alice;
    keypair_t bob;
    keypair_t mallory;
    TEST_ASSERT_EQUAL(ESP_OK, crypto_generate_keypair(&alice));
    TEST_ASSERT_EQUAL(ESP_OK, crypto_generate_keypair(&bob));
    TEST_ASSERT_EQUAL(ESP_OK, crypto_generate_keypair(&mallory));

    dag_t dag;
    TEST_ASSERT_EQUAL(ESP_OK, dag_init(&dag));

    transaction_t tx;
    make_transfer(&tx, &alice, &bob, 4, 4000);
    TEST_ASSERT_EQUAL(ESP_OK, dag_insert(&dag, &tx));

    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE,
        tx_lifecycle_confirm_by_ack(&dag, NULL, &tx.id,
                                    &mallory.public_key));
    TEST_ASSERT_EQUAL(TX_STATUS_LOCKED, must_get(&dag, &tx.id)->status);

    TEST_ASSERT_EQUAL(ESP_OK,
        tx_lifecycle_confirm_by_ack(&dag, NULL, &tx.id,
                                    &bob.public_key));
    TEST_ASSERT_EQUAL(TX_STATUS_CONFIRMED, must_get(&dag, &tx.id)->status);
}
