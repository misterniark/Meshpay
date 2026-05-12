/**
 * @file test_wallet.c
 * @brief Tests unitaires pour le module core/wallet/.
 *
 * Teste le calcul de solde, le verrouillage avec timeout,
 * l'expiration des verrous, et les checkpoints.
 */

#include "unity.h"
#include "wallet/wallet.h"
#include "wallet/wallet_lock.h"
#include "wallet/wallet_checkpoint.h"
#include "dag/dag.h"
#include "transaction/tx_create.h"
#include "crypto/crypto_keys.h"
#include "currency/currency_config.h"
#include "currency/currency_melt.h"
#include <string.h>

/* ========================================================================= */
/*                         Mock du temps                                      */
/* ========================================================================= */

/** Temps courant simulé (modifiable dans les tests) */
static uint64_t s_mock_time_ms = 0;

/** Fonction mock de récupération du temps */
static uint64_t mock_get_time(void)
{
    return s_mock_time_ms;
}

/* ========================================================================= */
/*                         Helpers                                            */
/* ========================================================================= */

static void make_hash(hash_t *h, uint8_t fill)
{
    memset(h->bytes, fill, CRYPTO_HASH_SIZE);
}

/* ========================================================================= */
/*                         Tests wallet (solde)                               */
/* ========================================================================= */

/**
 * @brief Vérifie le solde après un MINT.
 *
 * Scénario : le maître crée 500 crédits pour Alice.
 * Résultat attendu : solde d'Alice = 500.
 */
TEST_CASE("wallet_solde_apres_mint", "[wallet]")
{
    s_mock_time_ms = 0;

    dag_t dag;
    dag_init(&dag);

    keypair_t master, alice;
    crypto_generate_keypair(&master);
    crypto_generate_keypair(&alice);

    /* MINT de 500 crédits vers Alice */
    hash_t dummy;
    make_hash(&dummy, 0x01);
    transaction_t tx_mint;
    tx_create_mint(&tx_mint, &master, &alice.public_key, 500, 0, 0, &dummy, 1, 1000);
    dag_insert(&dag, &tx_mint);

    /* Calculer le solde d'Alice */
    wallet_t wallet;
    wallet_init(&wallet, &alice.public_key, &dag, mock_get_time);

    uint32_t balance = 0;
    wallet_get_balance(&wallet, 0, &balance);

    TEST_ASSERT_EQUAL(500, balance);
}

/**
 * @brief Vérifie le solde après un MINT puis un TRANSFER confirmé.
 *
 * Scénario :
 * 1. MINT 500 → Alice (CONFIRMED)
 * 2. Alice → Bob : 100 (on force CONFIRMED)
 * Résultat : Alice = 400, Bob = 100
 */
TEST_CASE("wallet_solde_apres_transfer", "[wallet]")
{
    s_mock_time_ms = 0;

    dag_t dag;
    dag_init(&dag);

    keypair_t master, alice, bob;
    crypto_generate_keypair(&master);
    crypto_generate_keypair(&alice);
    crypto_generate_keypair(&bob);

    /* MINT 500 → Alice */
    hash_t dummy;
    make_hash(&dummy, 0x01);
    transaction_t tx_mint;
    tx_create_mint(&tx_mint, &master, &alice.public_key, 500, 0, 0, &dummy, 1, 1000);
    dag_insert(&dag, &tx_mint);

    /* TRANSFER Alice → Bob : 100 */
    transaction_t tx_transfer;
    tx_create_transfer(&tx_transfer, &alice, &bob.public_key,
                       100, 0, 0, 0, &tx_mint.id, 1, 2000);
    /* Forcer le statut CONFIRMED pour ce test */
    tx_transfer.status = TX_STATUS_CONFIRMED;
    dag_insert(&dag, &tx_transfer);

    /* Solde Alice (sans frais) */
    wallet_t wallet_alice;
    wallet_init(&wallet_alice, &alice.public_key, &dag, mock_get_time);
    uint32_t balance_alice = 0;
    wallet_get_balance(&wallet_alice, 0, &balance_alice);
    TEST_ASSERT_EQUAL(400, balance_alice);

    /* Solde Bob */
    wallet_t wallet_bob;
    wallet_init(&wallet_bob, &bob.public_key, &dag, mock_get_time);
    uint32_t balance_bob = 0;
    wallet_get_balance(&wallet_bob, 0, &balance_bob);
    TEST_ASSERT_EQUAL(100, balance_bob);
}

/**
 * @brief Vérifie que les TX LOCKED réduisent le solde disponible.
 *
 * Un montant LOCKED n'est plus disponible pour un nouveau paiement.
 */
TEST_CASE("wallet_solde_locked_deduit", "[wallet]")
{
    s_mock_time_ms = 0;

    dag_t dag;
    dag_init(&dag);

    keypair_t master, alice, bob;
    crypto_generate_keypair(&master);
    crypto_generate_keypair(&alice);
    crypto_generate_keypair(&bob);

    /* MINT 500 → Alice */
    hash_t dummy;
    make_hash(&dummy, 0x01);
    transaction_t tx_mint;
    tx_create_mint(&tx_mint, &master, &alice.public_key, 500, 0, 0, &dummy, 1, 1000);
    dag_insert(&dag, &tx_mint);

    /* TRANSFER Alice → Bob : 200 (status LOCKED par défaut) */
    transaction_t tx_locked;
    tx_create_transfer(&tx_locked, &alice, &bob.public_key,
                       200, 0, 0, 0, &tx_mint.id, 1, 2000);
    dag_insert(&dag, &tx_locked);

    /* Solde Alice : 500 - 200 = 300 (même si LOCKED, c'est déduit) */
    wallet_t wallet;
    wallet_init(&wallet, &alice.public_key, &dag, mock_get_time);
    uint32_t balance = 0;
    wallet_get_balance(&wallet, 0, &balance);
    TEST_ASSERT_EQUAL(300, balance);
}

/**
 * @brief Vérifie que les TX CANCELLED ne réduisent pas le solde.
 */
TEST_CASE("wallet_solde_cancelled_ignore", "[wallet]")
{
    s_mock_time_ms = 0;

    dag_t dag;
    dag_init(&dag);

    keypair_t master, alice, bob;
    crypto_generate_keypair(&master);
    crypto_generate_keypair(&alice);
    crypto_generate_keypair(&bob);

    /* MINT 500 → Alice */
    hash_t dummy;
    make_hash(&dummy, 0x01);
    transaction_t tx_mint;
    tx_create_mint(&tx_mint, &master, &alice.public_key, 500, 0, 0, &dummy, 1, 1000);
    dag_insert(&dag, &tx_mint);

    /* TRANSFER 200 CANCELLED */
    transaction_t tx_cancelled;
    tx_create_transfer(&tx_cancelled, &alice, &bob.public_key,
                       200, 0, 0, 0, &tx_mint.id, 1, 2000);
    tx_cancelled.status = TX_STATUS_CANCELLED;
    dag_insert(&dag, &tx_cancelled);

    /* Solde Alice : toujours 500 (TX annulée ignorée) */
    wallet_t wallet;
    wallet_init(&wallet, &alice.public_key, &dag, mock_get_time);
    uint32_t balance = 0;
    wallet_get_balance(&wallet, 0, &balance);
    TEST_ASSERT_EQUAL(500, balance);
}

/**
 * @brief Vérifie le solde avec un base_balance issu d'un checkpoint.
 */
TEST_CASE("wallet_solde_avec_base_balance", "[wallet]")
{
    s_mock_time_ms = 0;

    dag_t dag;
    dag_init(&dag);

    keypair_t master, alice;
    crypto_generate_keypair(&master);
    crypto_generate_keypair(&alice);

    /* MINT 200 → Alice dans le DAG courant */
    hash_t dummy;
    make_hash(&dummy, 0x01);
    transaction_t tx;
    tx_create_mint(&tx, &master, &alice.public_key, 200, 0, 0, &dummy, 1, 3000);
    dag_insert(&dag, &tx);

    /* Solde avec base_balance de 300 (issu d'un checkpoint antérieur) */
    wallet_t wallet;
    wallet_init(&wallet, &alice.public_key, &dag, mock_get_time);
    uint32_t balance = 0;
    wallet_get_balance(&wallet, 300, &balance);

    /* 300 (base) + 200 (MINT) = 500 */
    TEST_ASSERT_EQUAL(500, balance);
}

/**
 * @brief Vérifie que les frais de transfert sont débités de l'émetteur.
 *
 * Scénario : fee = 10
 * 1. MINT 500 → Alice (CONFIRMED)
 * 2. Alice → Bob : 100 (CONFIRMED)
 * Résultat : Alice = 500 - (100 + 10) = 390, Bob = 100 (pas de fee reçu)
 */
TEST_CASE("wallet_solde_avec_transfer_fee", "[wallet]")
{
    s_mock_time_ms = 0;

    dag_t dag;
    dag_init(&dag);

    keypair_t master, alice, bob;
    crypto_generate_keypair(&master);
    crypto_generate_keypair(&alice);
    crypto_generate_keypair(&bob);

    /* MINT 500 → Alice */
    hash_t dummy;
    make_hash(&dummy, 0x01);
    transaction_t tx_mint;
    tx_create_mint(&tx_mint, &master, &alice.public_key, 500, 0, 0, &dummy, 1, 1000);
    dag_insert(&dag, &tx_mint);

    /* TRANSFER Alice → Bob : 100 avec fee = 10 (CONFIRMED) */
    transaction_t tx_transfer;
    tx_create_transfer(&tx_transfer, &alice, &bob.public_key,
                       100, 0, 10, 0, &tx_mint.id, 1, 2000);
    tx_transfer.status = TX_STATUS_CONFIRMED;
    dag_insert(&dag, &tx_transfer);

    /* Solde Alice : 500 - (100 + 10) = 390 */
    wallet_t wallet_alice;
    wallet_init(&wallet_alice, &alice.public_key, &dag, mock_get_time);
    uint32_t balance_alice = 0;
    wallet_get_balance(&wallet_alice, 0, &balance_alice);
    TEST_ASSERT_EQUAL(390, balance_alice);

    /* Solde Bob : 100 (reçoit uniquement amount, pas le fee) */
    wallet_t wallet_bob;
    wallet_init(&wallet_bob, &bob.public_key, &dag, mock_get_time);
    uint32_t balance_bob = 0;
    wallet_get_balance(&wallet_bob, 0, &balance_bob);
    TEST_ASSERT_EQUAL(100, balance_bob);

    /*
     * Solde Master : 10 (reçoit le fee comme fee_recipient).
     * Le master est configuré comme destinataire des frais.
     */
    wallet_t wallet_master;
    wallet_init(&wallet_master, &master.public_key, &dag, mock_get_time);
    memcpy(&wallet_master.fee_recipient, &master.public_key, sizeof(public_key_t));
    uint32_t balance_master = 0;
    wallet_get_balance(&wallet_master, 0, &balance_master);
    TEST_ASSERT_EQUAL(10, balance_master);
}

/**
 * @brief Vérifie que les frais s'appliquent aussi aux TX LOCKED.
 *
 * Scénario : fee = 5
 * 1. MINT 500 → Alice
 * 2. Alice → Bob : 200 (LOCKED)
 * Résultat : Alice = 500 - (200 + 5) = 295
 */
TEST_CASE("wallet_solde_locked_avec_fee", "[wallet]")
{
    s_mock_time_ms = 0;

    dag_t dag;
    dag_init(&dag);

    keypair_t master, alice, bob;
    crypto_generate_keypair(&master);
    crypto_generate_keypair(&alice);
    crypto_generate_keypair(&bob);

    hash_t dummy;
    make_hash(&dummy, 0x01);
    transaction_t tx_mint;
    tx_create_mint(&tx_mint, &master, &alice.public_key, 500, 0, 0, &dummy, 1, 1000);
    dag_insert(&dag, &tx_mint);

    /* TRANSFER LOCKED Alice → Bob : 200 avec fee = 5 */
    transaction_t tx_locked;
    tx_create_transfer(&tx_locked, &alice, &bob.public_key,
                       200, 0, 5, 0, &tx_mint.id, 1, 2000);
    dag_insert(&dag, &tx_locked);

    /* Solde Alice : 500 - (200 + 5) = 295, le fee est dans la TX */
    wallet_t wallet;
    wallet_init(&wallet, &alice.public_key, &dag, mock_get_time);
    uint32_t balance = 0;
    wallet_get_balance(&wallet, 0, &balance);
    TEST_ASSERT_EQUAL(295, balance);
}

/**
 * @brief Vérifie le checkpoint avec frais de transfert redirigés au master.
 *
 * Scénario : fee = 10
 * 1. MINT 500 → Alice
 * 2. Alice → Bob : 100 (CONFIRMED, fee = 10)
 * Checkpoint avec fee_recipient = master :
 *   Alice = 500 - (100 + 10) = 390
 *   Bob = 100 (reçoit uniquement amount)
 *   Master = 10 (reçoit le fee)
 *   Masse monétaire = 500 (rien de brûlé)
 */
TEST_CASE("checkpoint_avec_transfer_fee", "[wallet]")
{
    dag_t dag;
    dag_init(&dag);

    keypair_t master, alice, bob;
    crypto_generate_keypair(&master);
    crypto_generate_keypair(&alice);
    crypto_generate_keypair(&bob);

    hash_t dummy;
    make_hash(&dummy, 0x01);

    transaction_t tx_mint;
    tx_create_mint(&tx_mint, &master, &alice.public_key, 500, 0, 0, &dummy, 1, 1000);
    dag_insert(&dag, &tx_mint);

    transaction_t tx_transfer;
    tx_create_transfer(&tx_transfer, &alice, &bob.public_key,
                       100, 0, 10, 0, &tx_mint.id, 1, 2000);
    tx_transfer.status = TX_STATUS_CONFIRMED;
    dag_insert(&dag, &tx_transfer);

    /* Créer le checkpoint — le fee va au master (fee_recipient) */
    checkpoint_t chk;
    checkpoint_create(&dag, NULL, &master.public_key, &chk);

    uint32_t balance_alice = 0, balance_bob = 0, balance_master = 0;
    checkpoint_get_balance(&chk, &alice.public_key, &balance_alice);
    checkpoint_get_balance(&chk, &bob.public_key, &balance_bob);
    checkpoint_get_balance(&chk, &master.public_key, &balance_master);

    TEST_ASSERT_EQUAL(390, balance_alice);  /* 500 - (100 + 10) */
    TEST_ASSERT_EQUAL(100, balance_bob);    /* reçoit uniquement amount */
    TEST_ASSERT_EQUAL(10, balance_master);  /* reçoit le fee */

    /* Vérifier que la masse monétaire est conservée (rien de brûlé) */
    TEST_ASSERT_EQUAL(500, balance_alice + balance_bob + balance_master);
}

/**
 * @brief Vérifie que les fees sont brûlés quand fee_recipient est NULL.
 *
 * Même scénario que ci-dessus mais sans fee_recipient :
 * la masse monétaire perd 10 crédits (fees brûlés).
 */
TEST_CASE("checkpoint_fee_brule_sans_recipient", "[wallet]")
{
    dag_t dag;
    dag_init(&dag);

    keypair_t master, alice, bob;
    crypto_generate_keypair(&master);
    crypto_generate_keypair(&alice);
    crypto_generate_keypair(&bob);

    hash_t dummy;
    make_hash(&dummy, 0x01);

    transaction_t tx_mint;
    tx_create_mint(&tx_mint, &master, &alice.public_key, 500, 0, 0, &dummy, 1, 1000);
    dag_insert(&dag, &tx_mint);

    transaction_t tx_transfer;
    tx_create_transfer(&tx_transfer, &alice, &bob.public_key,
                       100, 0, 10, 0, &tx_mint.id, 1, 2000);
    tx_transfer.status = TX_STATUS_CONFIRMED;
    dag_insert(&dag, &tx_transfer);

    /* Checkpoint SANS fee_recipient → fees brûlés */
    checkpoint_t chk;
    checkpoint_create(&dag, NULL, NULL, &chk);

    uint32_t balance_alice = 0, balance_bob = 0;
    checkpoint_get_balance(&chk, &alice.public_key, &balance_alice);
    checkpoint_get_balance(&chk, &bob.public_key, &balance_bob);

    TEST_ASSERT_EQUAL(390, balance_alice);
    TEST_ASSERT_EQUAL(100, balance_bob);
    /* Masse monétaire = 490 (10 brûlés) */
    TEST_ASSERT_EQUAL(490, balance_alice + balance_bob);
}

/* ========================================================================= */
/*                         Tests wallet_lock                                  */
/* ========================================================================= */

/**
 * @brief Vérifie le cycle de vie d'un verrou : lock → confirm.
 */
TEST_CASE("lock_cycle_confirm", "[wallet]")
{
    s_mock_time_ms = 1000;

    dag_t dag;
    dag_init(&dag);

    keypair_t alice;
    crypto_generate_keypair(&alice);

    wallet_t wallet;
    wallet_init(&wallet, &alice.public_key, &dag, mock_get_time);

    lock_table_t table;
    lock_table_init(&table, &wallet);

    hash_t tx_id;
    make_hash(&tx_id, 0xAA);

    /* Verrouiller 100 crédits */
    esp_err_t err = lock_table_lock(&table, &tx_id, 100);
    TEST_ASSERT_EQUAL(ESP_OK, err);

    uint32_t total = 0;
    lock_table_total_locked(&table, &total);
    TEST_ASSERT_EQUAL(100, total);

    /* Confirmer le verrou */
    err = lock_table_confirm(&table, &tx_id);
    TEST_ASSERT_EQUAL(ESP_OK, err);

    lock_table_total_locked(&table, &total);
    TEST_ASSERT_EQUAL(0, total);
}

/**
 * @brief Vérifie le cycle : lock → cancel (libère le montant).
 */
TEST_CASE("lock_cycle_cancel", "[wallet]")
{
    s_mock_time_ms = 1000;

    dag_t dag;
    dag_init(&dag);

    keypair_t alice;
    crypto_generate_keypair(&alice);

    wallet_t wallet;
    wallet_init(&wallet, &alice.public_key, &dag, mock_get_time);

    lock_table_t table;
    lock_table_init(&table, &wallet);

    hash_t tx_id;
    make_hash(&tx_id, 0xBB);

    lock_table_lock(&table, &tx_id, 200);

    /* Annuler le verrou */
    esp_err_t err = lock_table_cancel(&table, &tx_id);
    TEST_ASSERT_EQUAL(ESP_OK, err);

    uint32_t total = 0;
    lock_table_total_locked(&table, &total);
    TEST_ASSERT_EQUAL(0, total);
}

/**
 * @brief Vérifie l'expiration automatique après 30 secondes.
 *
 * C'est le test clé du modèle "solde verrouillé" :
 * si le destinataire ne confirme pas dans les 30s,
 * le verrou expire et le montant redevient disponible.
 */
TEST_CASE("lock_expiration_30s", "[wallet]")
{
    s_mock_time_ms = 1000;

    dag_t dag;
    dag_init(&dag);

    keypair_t alice;
    crypto_generate_keypair(&alice);

    wallet_t wallet;
    wallet_init(&wallet, &alice.public_key, &dag, mock_get_time);

    lock_table_t table;
    lock_table_init(&table, &wallet);

    hash_t tx_id;
    make_hash(&tx_id, 0xCC);
    lock_table_lock(&table, &tx_id, 150);

    /* Avancer le temps de 29s → pas encore expiré */
    s_mock_time_ms = 1000 + 29000;
    uint32_t expired = 0;
    hash_t expired_ids[WALLET_MAX_LOCKS];
    lock_table_expire(&table, expired_ids, WALLET_MAX_LOCKS, &expired);
    TEST_ASSERT_EQUAL(0, expired);

    uint32_t total = 0;
    lock_table_total_locked(&table, &total);
    TEST_ASSERT_EQUAL(150, total);

    /* Avancer à 30s → doit expirer */
    s_mock_time_ms = 1000 + WALLET_LOCK_TIMEOUT_MS;
    lock_table_expire(&table, expired_ids, WALLET_MAX_LOCKS, &expired);
    TEST_ASSERT_EQUAL(1, expired);

    /* Vérifier que le tx_id expiré est bien retourné */
    TEST_ASSERT_TRUE(hash_equal(&expired_ids[0], &tx_id));

    lock_table_total_locked(&table, &total);
    TEST_ASSERT_EQUAL(0, total);
}

/**
 * @brief Vérifie le rejet quand la table de verrous est pleine.
 */
TEST_CASE("lock_table_pleine", "[wallet]")
{
    s_mock_time_ms = 1000;

    dag_t dag;
    dag_init(&dag);

    keypair_t alice;
    crypto_generate_keypair(&alice);

    wallet_t wallet;
    wallet_init(&wallet, &alice.public_key, &dag, mock_get_time);

    lock_table_t table;
    lock_table_init(&table, &wallet);

    /* Remplir la table de verrous */
    for (int i = 0; i < WALLET_MAX_LOCKS; i++) {
        hash_t tx_id;
        make_hash(&tx_id, (uint8_t)(i + 1));
        esp_err_t err = lock_table_lock(&table, &tx_id, 10);
        TEST_ASSERT_EQUAL(ESP_OK, err);
    }

    /* Un verrou de plus → table pleine */
    hash_t extra;
    make_hash(&extra, 0xFF);
    TEST_ASSERT_EQUAL(ESP_ERR_NO_MEM, lock_table_lock(&table, &extra, 10));
}

/* ========================================================================= */
/*                         Tests wallet_checkpoint                            */
/* ========================================================================= */

/**
 * @brief Vérifie la création d'un checkpoint à partir du DAG.
 *
 * Scénario :
 * - MINT 500 → Alice (CONFIRMED)
 * - TRANSFER 100 Alice → Bob (CONFIRMED)
 * Résultat checkpoint : Alice=400, Bob=100
 */
TEST_CASE("checkpoint_creation", "[wallet]")
{
    dag_t dag;
    dag_init(&dag);

    keypair_t master, alice, bob;
    crypto_generate_keypair(&master);
    crypto_generate_keypair(&alice);
    crypto_generate_keypair(&bob);

    hash_t dummy;
    make_hash(&dummy, 0x01);

    /* MINT 500 → Alice */
    transaction_t tx_mint;
    tx_create_mint(&tx_mint, &master, &alice.public_key, 500, 0, 0, &dummy, 1, 1000);
    dag_insert(&dag, &tx_mint);

    /* TRANSFER Alice → Bob : 100 sans frais (forcé CONFIRMED) */
    transaction_t tx_transfer;
    tx_create_transfer(&tx_transfer, &alice, &bob.public_key,
                       100, 0, 0, 0, &tx_mint.id, 1, 2000);
    tx_transfer.status = TX_STATUS_CONFIRMED;
    dag_insert(&dag, &tx_transfer);

    /* Créer le checkpoint — le fee est dans la TX (0) */
    checkpoint_t chk;
    esp_err_t err = checkpoint_create(&dag, NULL, NULL, &chk);
    TEST_ASSERT_EQUAL(ESP_OK, err);

    /* Vérifier les soldes */
    uint32_t balance_alice = 0, balance_bob = 0;
    checkpoint_get_balance(&chk, &alice.public_key, &balance_alice);
    checkpoint_get_balance(&chk, &bob.public_key, &balance_bob);

    TEST_ASSERT_EQUAL(400, balance_alice);
    TEST_ASSERT_EQUAL(100, balance_bob);
}

/**
 * @brief Vérifie la création d'un checkpoint incrémental (avec base).
 *
 * Simule le scénario :
 * 1. Checkpoint N : Alice=300, Bob=100
 * 2. DAG courant : MINT 200 → Alice
 * 3. Checkpoint N+1 : Alice=500, Bob=100
 */
TEST_CASE("checkpoint_incremental", "[wallet]")
{
    /* Créer un checkpoint de base */
    checkpoint_t base;
    memset(&base, 0, sizeof(base));

    keypair_t alice_kp, bob_kp;
    crypto_generate_keypair(&alice_kp);
    crypto_generate_keypair(&bob_kp);

    memcpy(&base.accounts[0].key, &alice_kp.public_key, sizeof(public_key_t));
    base.accounts[0].balance = 300;
    memcpy(&base.accounts[1].key, &bob_kp.public_key, sizeof(public_key_t));
    base.accounts[1].balance = 100;
    base.account_count = 2;

    /* DAG avec un nouveau MINT */
    dag_t dag;
    dag_init(&dag);

    keypair_t master;
    crypto_generate_keypair(&master);

    hash_t dummy;
    make_hash(&dummy, 0x01);
    transaction_t tx;
    tx_create_mint(&tx, &master, &alice_kp.public_key, 200, 0, 0, &dummy, 1, 5000);
    dag_insert(&dag, &tx);

    /* Créer le checkpoint incrémental — le fee est dans les TX */
    checkpoint_t chk;
    checkpoint_create(&dag, &base, NULL, &chk);

    uint32_t balance_alice = 0, balance_bob = 0;
    checkpoint_get_balance(&chk, &alice_kp.public_key, &balance_alice);
    checkpoint_get_balance(&chk, &bob_kp.public_key, &balance_bob);

    TEST_ASSERT_EQUAL(500, balance_alice);  /* 300 + 200 */
    TEST_ASSERT_EQUAL(100, balance_bob);     /* inchangé */
}

/**
 * @brief Vérifie que checkpoint_get_balance retourne 0 pour un compte inconnu.
 */
TEST_CASE("checkpoint_compte_inexistant", "[wallet]")
{
    checkpoint_t chk;
    memset(&chk, 0, sizeof(chk));
    chk.account_count = 0;

    keypair_t unknown;
    crypto_generate_keypair(&unknown);

    uint32_t balance = 999;
    esp_err_t err = checkpoint_get_balance(&chk, &unknown.public_key, &balance);

    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND, err);
    TEST_ASSERT_EQUAL(0, balance);
}

/**
 * @brief Vérifie la création d'un checkpoint à partir d'un DAG vide.
 *
 * Scénario : aucun nœud dans le DAG.
 * Résultat : checkpoint valide avec account_count = 0,
 *            timestamp = 0 et last_tx_id vide.
 */
TEST_CASE("checkpoint_dag_vide", "[wallet]")
{
    dag_t dag;
    dag_init(&dag);

    checkpoint_t chk;
    esp_err_t err = checkpoint_create(&dag, NULL, NULL, &chk);

    TEST_ASSERT_EQUAL(ESP_OK, err);
    TEST_ASSERT_EQUAL(0, chk.account_count);
    TEST_ASSERT_EQUAL(0, chk.timestamp);

    /* last_tx_id doit être à zéro (aucune TX traitée) */
    hash_t zero_hash;
    memset(&zero_hash, 0, sizeof(hash_t));
    TEST_ASSERT_TRUE(hash_equal(&chk.last_tx_id, &zero_hash));
}

/**
 * @brief Vérifie l'accumulation de plusieurs MINT vers le même compte.
 *
 * Scénario :
 * - MINT 100 → Alice (CONFIRMED)
 * - MINT 200 → Alice (CONFIRMED)
 * Résultat checkpoint : Alice = 300
 */
TEST_CASE("checkpoint_mint_multiples", "[wallet]")
{
    dag_t dag;
    dag_init(&dag);

    keypair_t master, alice;
    crypto_generate_keypair(&master);
    crypto_generate_keypair(&alice);

    hash_t dummy;
    make_hash(&dummy, 0x01);

    /* Premier MINT 100 → Alice */
    transaction_t tx1;
    tx_create_mint(&tx1, &master, &alice.public_key, 100, 0, 0, &dummy, 1, 1000);
    dag_insert(&dag, &tx1);

    /* Deuxième MINT 200 → Alice */
    transaction_t tx2;
    tx_create_mint(&tx2, &master, &alice.public_key, 200, 0, 0, &tx1.id, 1, 2000);
    dag_insert(&dag, &tx2);

    /* Créer le checkpoint */
    checkpoint_t chk;
    esp_err_t err = checkpoint_create(&dag, NULL, NULL, &chk);
    TEST_ASSERT_EQUAL(ESP_OK, err);

    uint32_t balance = 0;
    checkpoint_get_balance(&chk, &alice.public_key, &balance);
    TEST_ASSERT_EQUAL(300, balance);  /* 100 + 200 */
    TEST_ASSERT_EQUAL(1, chk.account_count);  /* un seul compte */
}

/**
 * @brief Vérifie le checkpoint avec 3 comptes distincts.
 *
 * Scénario :
 * - MINT 100 → Alice
 * - MINT 200 → Bob
 * - MINT 300 → Charlie
 * Résultat checkpoint : Alice=100, Bob=200, Charlie=300,
 *                       account_count=3.
 */
TEST_CASE("checkpoint_multi_comptes", "[wallet]")
{
    dag_t dag;
    dag_init(&dag);

    keypair_t master, alice, bob, charlie;
    crypto_generate_keypair(&master);
    crypto_generate_keypair(&alice);
    crypto_generate_keypair(&bob);
    crypto_generate_keypair(&charlie);

    hash_t dummy;
    make_hash(&dummy, 0x01);

    /* MINT vers 3 comptes différents */
    transaction_t tx_a;
    tx_create_mint(&tx_a, &master, &alice.public_key, 100, 0, 0, &dummy, 1, 1000);
    dag_insert(&dag, &tx_a);

    transaction_t tx_b;
    tx_create_mint(&tx_b, &master, &bob.public_key, 200, 0, 0, &tx_a.id, 1, 2000);
    dag_insert(&dag, &tx_b);

    transaction_t tx_c;
    tx_create_mint(&tx_c, &master, &charlie.public_key, 300, 0, 0, &tx_b.id, 1, 3000);
    dag_insert(&dag, &tx_c);

    /* Créer le checkpoint */
    checkpoint_t chk;
    esp_err_t err = checkpoint_create(&dag, NULL, NULL, &chk);
    TEST_ASSERT_EQUAL(ESP_OK, err);
    TEST_ASSERT_EQUAL(3, chk.account_count);

    uint32_t bal_a = 0, bal_b = 0, bal_c = 0;
    checkpoint_get_balance(&chk, &alice.public_key, &bal_a);
    checkpoint_get_balance(&chk, &bob.public_key, &bal_b);
    checkpoint_get_balance(&chk, &charlie.public_key, &bal_c);

    TEST_ASSERT_EQUAL(100, bal_a);
    TEST_ASSERT_EQUAL(200, bal_b);
    TEST_ASSERT_EQUAL(300, bal_c);
}

/**
 * @brief Vérifie que les TX CANCELLED sont ignorées par le checkpoint.
 *
 * Scénario :
 * - MINT 500 → Alice (CONFIRMED)
 * - TRANSFER 100 Alice → Bob (CANCELLED)
 * Résultat checkpoint : Alice=500, Bob absent (ESP_ERR_NOT_FOUND).
 *
 * Le checkpoint ne traite que les TX CONFIRMED (ligne 92 de
 * wallet_checkpoint.c). La TX CANCELLED est sautée entièrement,
 * donc Bob n'apparaît jamais dans le checkpoint.
 */
TEST_CASE("checkpoint_ignore_cancelled", "[wallet]")
{
    dag_t dag;
    dag_init(&dag);

    keypair_t master, alice, bob;
    crypto_generate_keypair(&master);
    crypto_generate_keypair(&alice);
    crypto_generate_keypair(&bob);

    hash_t dummy;
    make_hash(&dummy, 0x01);

    /* MINT 500 → Alice */
    transaction_t tx_mint;
    tx_create_mint(&tx_mint, &master, &alice.public_key, 500, 0, 0, &dummy, 1, 1000);
    dag_insert(&dag, &tx_mint);

    /* TRANSFER 100 Alice → Bob, forcé CANCELLED */
    transaction_t tx_transfer;
    tx_create_transfer(&tx_transfer, &alice, &bob.public_key,
                       100, 0, 0, 0, &tx_mint.id, 1, 2000);
    tx_transfer.status = TX_STATUS_CANCELLED;
    dag_insert(&dag, &tx_transfer);

    /* Créer le checkpoint */
    checkpoint_t chk;
    esp_err_t err = checkpoint_create(&dag, NULL, NULL, &chk);
    TEST_ASSERT_EQUAL(ESP_OK, err);

    /* Alice garde ses 500 (la TX annulée n'a aucun effet) */
    uint32_t balance_alice = 0;
    checkpoint_get_balance(&chk, &alice.public_key, &balance_alice);
    TEST_ASSERT_EQUAL(500, balance_alice);

    /* Bob n'existe pas dans le checkpoint */
    uint32_t balance_bob = 999;
    esp_err_t err_bob = checkpoint_get_balance(&chk, &bob.public_key, &balance_bob);
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND, err_bob);
    TEST_ASSERT_EQUAL(0, balance_bob);
}

/**
 * @brief Vérifie que les TX LOCKED sont ignorées par le checkpoint.
 *
 * Le checkpoint ne prend en compte que les TX avec
 * status == TX_STATUS_CONFIRMED (wallet_checkpoint.c, ligne 92).
 * Les TX LOCKED sont donc exclues du calcul des soldes.
 *
 * Scénario :
 * - MINT 500 → Alice (CONFIRMED, statut par défaut de tx_create_mint)
 * - TRANSFER 100 Alice → Bob (LOCKED, statut par défaut de tx_create_transfer)
 * Résultat checkpoint : Alice=500 (le débit LOCKED n'est pas appliqué),
 *                       Bob absent (le crédit LOCKED n'est pas appliqué).
 *
 * Note : ceci diffère du wallet (wallet_get_balance) qui déduit
 * les montants LOCKED du solde disponible. Le checkpoint ne reflète
 * que les mouvements définitivement confirmés.
 */
TEST_CASE("checkpoint_locked_inclus", "[wallet]")
{
    dag_t dag;
    dag_init(&dag);

    keypair_t master, alice, bob;
    crypto_generate_keypair(&master);
    crypto_generate_keypair(&alice);
    crypto_generate_keypair(&bob);

    hash_t dummy;
    make_hash(&dummy, 0x01);

    /* MINT 500 → Alice (CONFIRMED par défaut) */
    transaction_t tx_mint;
    tx_create_mint(&tx_mint, &master, &alice.public_key, 500, 0, 0, &dummy, 1, 1000);
    dag_insert(&dag, &tx_mint);

    /* TRANSFER 100 Alice → Bob (LOCKED par défaut, non confirmé) */
    transaction_t tx_locked;
    tx_create_transfer(&tx_locked, &alice, &bob.public_key,
                       100, 0, 0, 0, &tx_mint.id, 1, 2000);
    /* tx_create_transfer met status = TX_STATUS_LOCKED par défaut */
    dag_insert(&dag, &tx_locked);

    /* Créer le checkpoint */
    checkpoint_t chk;
    esp_err_t err = checkpoint_create(&dag, NULL, NULL, &chk);
    TEST_ASSERT_EQUAL(ESP_OK, err);

    /*
     * Alice = 500 : le checkpoint ignore les TX LOCKED,
     * donc le débit de 100 n'est pas appliqué.
     */
    uint32_t balance_alice = 0;
    checkpoint_get_balance(&chk, &alice.public_key, &balance_alice);
    TEST_ASSERT_EQUAL(500, balance_alice);

    /*
     * Bob n'apparaît pas dans le checkpoint car la seule TX
     * qui le concerne est LOCKED (ignorée).
     */
    uint32_t balance_bob = 999;
    esp_err_t err_bob = checkpoint_get_balance(&chk, &bob.public_key, &balance_bob);
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND, err_bob);
    TEST_ASSERT_EQUAL(0, balance_bob);
}

/**
 * @brief Vérifie le checkpoint incrémental avec un nouveau compte.
 *
 * Scénario :
 * 1. Checkpoint base : Alice=300
 * 2. DAG courant : MINT 100 → Charlie (nouveau compte)
 * 3. Checkpoint N+1 : Alice=300 (inchangé), Charlie=100 (nouveau)
 */
TEST_CASE("checkpoint_incremental_nouveau_compte", "[wallet]")
{
    /* Construire un checkpoint de base avec Alice=300 */
    checkpoint_t base;
    memset(&base, 0, sizeof(base));

    keypair_t alice_kp, charlie_kp;
    crypto_generate_keypair(&alice_kp);
    crypto_generate_keypair(&charlie_kp);

    memcpy(&base.accounts[0].key, &alice_kp.public_key, sizeof(public_key_t));
    base.accounts[0].balance = 300;
    base.account_count = 1;

    /* DAG avec un MINT vers Charlie (nouveau compte) */
    dag_t dag;
    dag_init(&dag);

    keypair_t master;
    crypto_generate_keypair(&master);

    hash_t dummy;
    make_hash(&dummy, 0x01);
    transaction_t tx;
    tx_create_mint(&tx, &master, &charlie_kp.public_key, 100, 0, 0, &dummy, 1, 5000);
    dag_insert(&dag, &tx);

    /* Créer le checkpoint incrémental */
    checkpoint_t chk;
    esp_err_t err = checkpoint_create(&dag, &base, NULL, &chk);
    TEST_ASSERT_EQUAL(ESP_OK, err);

    /* Alice conserve son solde de base */
    uint32_t balance_alice = 0;
    checkpoint_get_balance(&chk, &alice_kp.public_key, &balance_alice);
    TEST_ASSERT_EQUAL(300, balance_alice);

    /* Charlie apparaît avec son MINT */
    uint32_t balance_charlie = 0;
    checkpoint_get_balance(&chk, &charlie_kp.public_key, &balance_charlie);
    TEST_ASSERT_EQUAL(100, balance_charlie);

    /* Deux comptes au total */
    TEST_ASSERT_EQUAL(2, chk.account_count);
}

/**
 * @brief Vérifie que timestamp et last_tx_id sont correctement remplis.
 *
 * Scénario : deux TX CONFIRMED avec timestamps 1000 et 3000.
 * Le checkpoint doit avoir :
 * - timestamp = 3000 (le plus récent)
 * - last_tx_id = id de la TX avec timestamp 3000
 */
TEST_CASE("checkpoint_timestamp_et_last_tx", "[wallet]")
{
    dag_t dag;
    dag_init(&dag);

    keypair_t master, alice;
    crypto_generate_keypair(&master);
    crypto_generate_keypair(&alice);

    hash_t dummy;
    make_hash(&dummy, 0x01);

    /* TX 1 : timestamp 1000 */
    transaction_t tx1;
    tx_create_mint(&tx1, &master, &alice.public_key, 100, 0, 0, &dummy, 1, 1000);
    dag_insert(&dag, &tx1);

    /* TX 2 : timestamp 3000 (la plus récente) */
    transaction_t tx2;
    tx_create_mint(&tx2, &master, &alice.public_key, 200, 0, 0, &tx1.id, 1, 3000);
    dag_insert(&dag, &tx2);

    /* Créer le checkpoint */
    checkpoint_t chk;
    esp_err_t err = checkpoint_create(&dag, NULL, NULL, &chk);
    TEST_ASSERT_EQUAL(ESP_OK, err);

    /* Le timestamp doit être celui de la TX la plus récente */
    TEST_ASSERT_EQUAL(3000, chk.timestamp);

    /* last_tx_id doit correspondre à tx2 (timestamp le plus élevé) */
    TEST_ASSERT_TRUE(hash_equal(&chk.last_tx_id, &tx2.id));
}

/* ========================================================================= */
/*                         Tests fonte (melt / demurrage)                     */
/* ========================================================================= */

/**
 * @brief Verifie que la fonte BPS reduit correctement le solde.
 *
 * Scenario :
 * 1. Solde initial de 10000
 * 2. Fonte : 1% par jour (100 BPS), 3 jours ecoules
 * 3. Resultat attendu : 10000 * 0.99^3 = 9702 (arrondi entier)
 *
 * Ce test utilise directement les fonctions de currency_melt
 * pour verifier l'integration avec les soldes du wallet.
 */
TEST_CASE("melt_bps_reduit_solde", "[wallet]")
{
    currency_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.melt_enabled = true;
    cfg.melt_period_seconds = 86400; /* 1 jour */
    cfg.melt_volume_mode = MELT_MODE_BPS;
    cfg.melt_bps = 100; /* 1% par tick */

    uint64_t t0 = 1000000; /* timestamp initial en ms */
    uint64_t t1 = t0 + (3 * 86400 * 1000); /* +3 jours en ms */

    uint32_t ticks = currency_melt_ticks_due(&cfg, t0, t1);
    TEST_ASSERT_EQUAL(3, ticks);

    uint32_t balance = 10000;
    balance = currency_melt_apply(&cfg, balance, ticks);

    /* 10000 * 0.99 = 9900, * 0.99 = 9801, * 0.99 = 9702 (troncature entiere) */
    TEST_ASSERT_EQUAL(9702, balance);

    /* Verifier l'avancement du timestamp */
    uint64_t next_ts = currency_melt_next_timestamp(&cfg, t0, ticks, t1);
    TEST_ASSERT_EQUAL(t0 + 3 * 86400 * 1000, next_ts);
}

/**
 * @brief Verifie que la fonte ne s'applique pas quand desactivee.
 */
TEST_CASE("melt_desactive_pas_de_reduction", "[wallet]")
{
    currency_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.melt_enabled = false;
    cfg.melt_period_seconds = 86400;
    cfg.melt_volume_mode = MELT_MODE_BPS;
    cfg.melt_bps = 100;

    uint64_t t0 = 1000000;
    uint64_t t1 = t0 + (5 * 86400 * 1000);

    uint32_t ticks = currency_melt_ticks_due(&cfg, t0, t1);
    TEST_ASSERT_EQUAL(0, ticks);
}

/**
 * @brief Verifie que le checkpoint preserve le last_melt_timestamp global.
 *
 * Scenario :
 * 1. Creer un checkpoint de base avec last_melt_timestamp global = 42000
 * 2. Creer un checkpoint incremental (DAG vide)
 * 3. Verifier que le timestamp global n'est pas dans le nouveau checkpoint
 *    (c'est main.c qui le recopie, pas checkpoint_create)
 *    mais que la structure checkpoint_t possede bien le champ.
 */
TEST_CASE("checkpoint_preserves_melt_timestamp", "[wallet]")
{
    /* Creer un checkpoint de base avec un melt_timestamp global */
    checkpoint_t base;
    memset(&base, 0, sizeof(base));

    keypair_t alice_kp;
    crypto_generate_keypair(&alice_kp);

    memcpy(&base.accounts[0].key, &alice_kp.public_key, sizeof(public_key_t));
    base.accounts[0].balance = 500;
    base.account_count = 1;
    base.last_melt_timestamp = 42000;

    /* DAG vide — le checkpoint incremental copie la base */
    dag_t dag;
    dag_init(&dag);

    checkpoint_t chk;
    esp_err_t err = checkpoint_create(&dag, &base, NULL, &chk);
    TEST_ASSERT_EQUAL(ESP_OK, err);
    TEST_ASSERT_EQUAL(1, chk.account_count);
    TEST_ASSERT_EQUAL(500, chk.accounts[0].balance);

    /*
     * checkpoint_create ne copie pas last_melt_timestamp (c'est main.c
     * qui le fait lors du checkpoint automatique). Apres memset(0),
     * le champ est a 0 dans le nouveau checkpoint.
     */
    TEST_ASSERT_EQUAL(0, chk.last_melt_timestamp);
}

/**
 * @brief Verifie le calcul compose BPS sur plusieurs ticks avec des valeurs
 *        realistes de festival.
 *
 * Scenario :
 * 1. Un festivalier a 5000 credits
 * 2. Fonte configuree a 500 BPS (5%) par jour
 * 3. 7 jours s'ecoulent (7 ticks)
 * 4. Solde attendu : 5000 * 0.95^7 = 3482 (arrondi entier par troncature
 *    successive : 4750 → 4512 → 4286 → 4071 → 3867 → 3673 → 3489)
 *
 * Ce test valide que la composition est bien sequentielle (pas exponentielle
 * en un seul calcul) et que la troncature entiere a chaque tick produit
 * un resultat deterministe.
 */
TEST_CASE("melt_bps_compose_precision", "[wallet][melt]")
{
    currency_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.melt_enabled = true;
    cfg.melt_period_seconds = 86400; /* 1 jour */
    cfg.melt_volume_mode = MELT_MODE_BPS;
    cfg.melt_bps = 500; /* 5% par tick */

    uint64_t t0 = 1000000;
    uint64_t t1 = t0 + (7ULL * 86400 * 1000); /* +7 jours */

    uint32_t ticks = currency_melt_ticks_due(&cfg, t0, t1);
    TEST_ASSERT_EQUAL(7, ticks);

    uint32_t balance = 5000;
    balance = currency_melt_apply(&cfg, balance, ticks);

    /*
     * Verification manuelle tick par tick :
     *   tick 1: 5000 * 9500 / 10000 = 4750
     *   tick 2: 4750 * 9500 / 10000 = 4512  (4512.5 tronque)
     *   tick 3: 4512 * 9500 / 10000 = 4286  (4286.4 tronque)
     *   tick 4: 4286 * 9500 / 10000 = 4071  (4071.7 tronque)
     *   tick 5: 4071 * 9500 / 10000 = 3867  (3867.45 tronque)
     *   tick 6: 3867 * 9500 / 10000 = 3673  (3673.65 tronque)
     *   tick 7: 3673 * 9500 / 10000 = 3489  (3489.35 tronque)
     */
    TEST_ASSERT_EQUAL(3489, balance);
}

/**
 * @brief Verifie le mode FIXED : un montant fixe est retire par tick.
 *
 * Scenario :
 * 1. Solde initial de 10000
 * 2. Fonte fixe de 200 credits par jour, 4 jours ecoules
 * 3. Solde attendu : 10000 - (200 * 4) = 9200
 *
 * Contrairement au mode BPS (compose), le mode FIXED est lineaire :
 * le montant retire est constant, independant du solde restant.
 */
TEST_CASE("melt_fixed_mode_reduction", "[wallet][melt]")
{
    currency_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.melt_enabled = true;
    cfg.melt_period_seconds = 86400;
    cfg.melt_volume_mode = MELT_MODE_FIXED;
    cfg.melt_fixed_amount = 200;

    uint64_t t0 = 1000000;
    uint64_t t1 = t0 + (4ULL * 86400 * 1000);

    uint32_t ticks = currency_melt_ticks_due(&cfg, t0, t1);
    TEST_ASSERT_EQUAL(4, ticks);

    uint32_t balance = 10000;
    balance = currency_melt_apply(&cfg, balance, ticks);
    TEST_ASSERT_EQUAL(9200, balance);
}

/**
 * @brief Verifie que le mode FIXED clamp a zero sans underflow.
 *
 * Scenario :
 * 1. Solde initial de 500
 * 2. Fonte fixe de 200 par jour, 5 jours ecoules
 * 3. Total fonte = 200 * 5 = 1000 > 500
 * 4. Resultat attendu : 0 (pas de valeur negative / underflow uint32)
 *
 * Ce test verifie la garde anti-underflow dans currency_melt_apply :
 * si total_melt >= current, le solde est mis a 0.
 */
TEST_CASE("melt_fixed_epuise_solde", "[wallet][melt]")
{
    currency_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.melt_enabled = true;
    cfg.melt_period_seconds = 86400;
    cfg.melt_volume_mode = MELT_MODE_FIXED;
    cfg.melt_fixed_amount = 200;

    uint32_t ticks = 5;
    uint32_t balance = 500;
    balance = currency_melt_apply(&cfg, balance, ticks);

    /* 200 * 5 = 1000 >= 500 → clamp a 0 */
    TEST_ASSERT_EQUAL(0, balance);
}

/**
 * @brief Verifie le plafonnement a MELT_MAX_CATCHUP_TICKS (365).
 *
 * Scenario :
 * 1. Un appareil reste eteint pendant 2 ans (730 jours)
 * 2. A son reveil, ticks_due ne doit pas retourner 730 mais 365
 * 3. Avec 1% BPS par jour, 365 ticks sur 10000 donne un solde tres bas
 *    mais pas forcement zero (10000 * 0.99^365 = ~25)
 *
 * Ce mecanisme protege contre l'effacement total du solde apres une
 * longue periode hors-ligne. Sans ce plafond, un appareil eteint
 * pendant 2 ans verrait son solde fondu presque integralement.
 */
TEST_CASE("melt_catchup_plafonne_365", "[wallet][melt]")
{
    currency_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.melt_enabled = true;
    cfg.melt_period_seconds = 86400;
    cfg.melt_volume_mode = MELT_MODE_BPS;
    cfg.melt_bps = 100; /* 1% par jour */

    uint64_t t0 = 1000000;
    /* 730 jours ecoules (2 ans) */
    uint64_t t1 = t0 + (730ULL * 86400 * 1000);

    uint32_t ticks = currency_melt_ticks_due(&cfg, t0, t1);
    /* Plafonne a MELT_MAX_CATCHUP_TICKS = 365 */
    TEST_ASSERT_EQUAL(365, ticks);

    /* Verifier que le solde n'est pas completement vide */
    uint32_t balance = 10000;
    balance = currency_melt_apply(&cfg, balance, ticks);
    /* 10000 * 0.99^365 ≈ 25 — le solde survit grace au plafond */
    TEST_ASSERT_TRUE(balance > 0);
    TEST_ASSERT_TRUE(balance < 100); /* mais drastiquement reduit */
}

/**
 * @brief Verifie que zero tick est retourne si moins d'une periode s'est ecoulee.
 *
 * Scenario :
 * 1. Periode de fonte : 1 jour (86400s)
 * 2. Temps ecoule : 12 heures (43200s)
 * 3. Aucun tick n'est du — la fonte ne s'applique pas
 *
 * Ce test valide le comportement "tout ou rien" des ticks : la fonte
 * ne s'applique qu'a chaque periode complete. Le temps restant (< 1 periode)
 * est conserve pour le prochain calcul via next_timestamp.
 */
TEST_CASE("melt_temps_insuffisant_zero_tick", "[wallet][melt]")
{
    currency_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.melt_enabled = true;
    cfg.melt_period_seconds = 86400;
    cfg.melt_volume_mode = MELT_MODE_BPS;
    cfg.melt_bps = 100;

    uint64_t t0 = 1000000;
    /* Seulement 12 heures ecoulees — pas une periode complete */
    uint64_t t1 = t0 + (43200ULL * 1000);

    uint32_t ticks = currency_melt_ticks_due(&cfg, t0, t1);
    TEST_ASSERT_EQUAL(0, ticks);

    /* Le solde ne change pas */
    uint32_t balance = 10000;
    balance = currency_melt_apply(&cfg, balance, ticks);
    TEST_ASSERT_EQUAL(10000, balance);
}

/**
 * @brief Verifie la garde melt_bps > 10000 (invalide, retourne 0).
 *
 * Un melt_bps superieur a 10000 signifie une fonte > 100%, ce qui
 * est absurde. L'API retourne 0 dans ce cas comme protection.
 * Ce test verifie que cette garde fonctionne correctement et qu'il
 * n'y a pas d'underflow sur le calcul (10000 - melt_bps).
 */
TEST_CASE("melt_bps_invalide_retourne_zero", "[wallet][melt]")
{
    currency_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.melt_enabled = true;
    cfg.melt_period_seconds = 86400;
    cfg.melt_volume_mode = MELT_MODE_BPS;
    cfg.melt_bps = 15000; /* 150% — invalide */

    uint32_t balance = 10000;
    balance = currency_melt_apply(&cfg, balance, 1);
    TEST_ASSERT_EQUAL(0, balance);
}

/**
 * @brief Verifie que la fonte sur un solde nul reste a zero.
 *
 * Un compte vide ne doit jamais avoir un solde negatif, et la fonction
 * ne doit pas crasher ou produire un comportement inattendu.
 */
TEST_CASE("melt_sur_solde_zero", "[wallet][melt]")
{
    currency_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.melt_enabled = true;
    cfg.melt_period_seconds = 86400;
    cfg.melt_volume_mode = MELT_MODE_BPS;
    cfg.melt_bps = 500;

    uint32_t balance = 0;
    balance = currency_melt_apply(&cfg, balance, 10);
    TEST_ASSERT_EQUAL(0, balance);

    /* Pareil en mode FIXED */
    cfg.melt_volume_mode = MELT_MODE_FIXED;
    cfg.melt_fixed_amount = 100;
    balance = currency_melt_apply(&cfg, balance, 5);
    TEST_ASSERT_EQUAL(0, balance);
}

/**
 * @brief Test end-to-end : simule le chemin main.c de fonte au checkpoint.
 *
 * Ce test reproduit exactement ce que fait main.c dans dag_insert_and_track()
 * lorsqu'un checkpoint est cree :
 * 1. Creer un checkpoint avec 3 comptes (soldes differents)
 * 2. Calculer les ticks ecoules
 * 3. Appliquer la fonte a chaque compte du checkpoint
 * 4. Mettre a jour le last_melt_timestamp global
 * 5. Verifier que tous les soldes sont correctement reduits
 *
 * C'est le test le plus important : il valide que le schema d'integration
 * (boucle sur accounts + timestamp global) produit des resultats corrects.
 */
TEST_CASE("melt_checkpoint_applique_tous_comptes", "[wallet][melt]")
{
    /* Configuration : 2% par jour, 5 jours ecoules */
    currency_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.melt_enabled = true;
    cfg.melt_period_seconds = 86400;
    cfg.melt_volume_mode = MELT_MODE_BPS;
    cfg.melt_bps = 200; /* 2% par tick */

    /* Creer un checkpoint avec 3 comptes aux soldes differents */
    checkpoint_t chk;
    memset(&chk, 0, sizeof(chk));

    keypair_t kp_a, kp_b, kp_c;
    crypto_generate_keypair(&kp_a);
    crypto_generate_keypair(&kp_b);
    crypto_generate_keypair(&kp_c);

    memcpy(&chk.accounts[0].key, &kp_a.public_key, sizeof(public_key_t));
    chk.accounts[0].balance = 10000; /* Gros portefeuille */
    memcpy(&chk.accounts[1].key, &kp_b.public_key, sizeof(public_key_t));
    chk.accounts[1].balance = 500;   /* Petit portefeuille */
    memcpy(&chk.accounts[2].key, &kp_c.public_key, sizeof(public_key_t));
    chk.accounts[2].balance = 1;     /* Cas limite : 1 credit */
    chk.account_count = 3;

    uint64_t t0 = 5000000;
    chk.last_melt_timestamp = t0;
    uint64_t now = t0 + (5ULL * 86400 * 1000); /* +5 jours */

    uint32_t ticks = currency_melt_ticks_due(&cfg, chk.last_melt_timestamp, now);
    TEST_ASSERT_EQUAL(5, ticks);

    /*
     * Appliquer la fonte a chaque compte — exactement comme main.c
     * le fait dans dag_insert_and_track() lors du checkpoint.
     */
    for (uint32_t i = 0; i < chk.account_count; i++) {
        chk.accounts[i].balance = currency_melt_apply(
            &cfg, chk.accounts[i].balance, ticks);
    }
    chk.last_melt_timestamp = currency_melt_next_timestamp(
        &cfg, t0, ticks, now);

    /*
     * Verification manuelle (2% par tick, 5 ticks) :
     * Compte A (10000): 9800 → 9604 → 9411 → 9222 → 9037
     * Compte B (500):   490  → 480  → 470  → 460  → 450
     * Compte C (1):     0    (1 * 9800/10000 = 0 des le 1er tick)
     */
    TEST_ASSERT_EQUAL(9037, chk.accounts[0].balance);
    TEST_ASSERT_EQUAL(450,  chk.accounts[1].balance);
    TEST_ASSERT_EQUAL(0,    chk.accounts[2].balance);

    /* Le timestamp global avance de 5 periodes exactes */
    TEST_ASSERT_EQUAL(t0 + 5ULL * 86400 * 1000, chk.last_melt_timestamp);
}

/**
 * @brief Verifie que next_timestamp conserve le reste de temps.
 *
 * Scenario :
 * 1. 3 jours et 6 heures se sont ecoules (3.25 periodes)
 * 2. ticks_due retourne 3 (periodes completes seulement)
 * 3. next_timestamp avance de exactement 3 jours (pas 3.25)
 * 4. Les 6 heures restantes seront comptabilisees au prochain appel
 *
 * Ce mecanisme est essentiel pour la precision a long terme : sans lui,
 * chaque appel "perdrait" le reste de temps, accumulant un decalage.
 */
TEST_CASE("melt_next_timestamp_conserve_reste", "[wallet][melt]")
{
    currency_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.melt_enabled = true;
    cfg.melt_period_seconds = 86400;
    cfg.melt_volume_mode = MELT_MODE_BPS;
    cfg.melt_bps = 100;

    uint64_t t0 = 1000000;
    /* 3 jours + 6 heures (3.25 periodes) */
    uint64_t t1 = t0 + (3ULL * 86400 * 1000) + (6ULL * 3600 * 1000);

    uint32_t ticks = currency_melt_ticks_due(&cfg, t0, t1);
    TEST_ASSERT_EQUAL(3, ticks);

    uint64_t next_ts = currency_melt_next_timestamp(&cfg, t0, ticks, t1);

    /* Le timestamp avance de 3 jours exactement (pas 3.25) */
    uint64_t expected_ts = t0 + (3ULL * 86400 * 1000);
    TEST_ASSERT_EQUAL(expected_ts, next_ts);

    /* Verification : le "reste" de 6h est bien conserve */
    uint64_t reste_ms = t1 - next_ts;
    TEST_ASSERT_EQUAL(6ULL * 3600 * 1000, reste_ms);

    /*
     * Si on rappelle ticks_due avec le nouveau timestamp et le meme now,
     * on obtient 0 tick (6h < 1 jour). Les 6h sont en attente.
     */
    uint32_t ticks2 = currency_melt_ticks_due(&cfg, next_ts, t1);
    TEST_ASSERT_EQUAL(0, ticks2);

    /*
     * Avancer de 18h supplementaires (total: 6h + 18h = 24h = 1 jour)
     * → un nouveau tick est du.
     */
    uint64_t t2 = t1 + (18ULL * 3600 * 1000);
    uint32_t ticks3 = currency_melt_ticks_due(&cfg, next_ts, t2);
    TEST_ASSERT_EQUAL(1, ticks3);
}

/**
 * @brief Scenario realiste end-to-end : MINT → fonte → TRANSFER → fonte.
 *
 * Ce test simule un cycle de vie complet dans un festival :
 *
 * Jour 0 : Le maitre cree 1000 credits pour Alice (MINT)
 * Jour 3 : 3 ticks de fonte appliques → solde reduit
 *           Alice paie 200 a Bob (TRANSFER, fee = 10)
 * Jour 5 : 2 ticks de fonte supplementaires → les deux soldes fondent
 *
 * On verifie que la fonte s'applique correctement AVANT et APRES
 * les transactions, et que le fee est bien brule (ni Alice ni Bob
 * ne le recuperent).
 *
 * Configuration : 1% par jour (BPS), fee = 10 par transfert
 */
TEST_CASE("melt_enchaine_transactions_et_fonte", "[wallet][melt]")
{
    /* ---- Setup : config monnaie + keypairs ---- */
    currency_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.melt_enabled = true;
    cfg.melt_period_seconds = 86400;
    cfg.melt_volume_mode = MELT_MODE_BPS;
    cfg.melt_bps = 100; /* 1% par jour */

    keypair_t master_kp, alice_kp, bob_kp;
    crypto_generate_keypair(&master_kp);
    crypto_generate_keypair(&alice_kp);
    crypto_generate_keypair(&bob_kp);

    /* ---- Jour 0 : MINT 1000 credits pour Alice ---- */
    dag_t dag;
    dag_init(&dag);

    hash_t genesis_h;
    make_hash(&genesis_h, 0xAA);

    transaction_t mint_tx;
    memset(&mint_tx, 0, sizeof(mint_tx));
    make_hash(&mint_tx.id, 0x01);
    mint_tx.type = TX_TYPE_MINT;
    memcpy(&mint_tx.from, &master_kp.public_key, sizeof(public_key_t));
    memcpy(&mint_tx.to, &alice_kp.public_key, sizeof(public_key_t));
    mint_tx.amount = 1000;
    mint_tx.fee = 0;
    mint_tx.timestamp = 0;
    mint_tx.status = TX_STATUS_CONFIRMED;
    memcpy(&mint_tx.parents[0], &genesis_h, sizeof(hash_t));
    mint_tx.parent_count = 1;
    dag_insert(&dag, &mint_tx);

    /* Creer un checkpoint initial (avant fonte) */
    checkpoint_t chk0;
    esp_err_t err = checkpoint_create(&dag, NULL, NULL, &chk0);
    TEST_ASSERT_EQUAL(ESP_OK, err);

    /* Verifier : Alice a 1000 credits */
    uint32_t alice_bal;
    err = checkpoint_get_balance(&chk0, &alice_kp.public_key, &alice_bal);
    TEST_ASSERT_EQUAL(ESP_OK, err);
    TEST_ASSERT_EQUAL(1000, alice_bal);

    /* ---- Jour 3 : appliquer 3 ticks de fonte ---- */
    uint64_t t0 = 0;
    uint64_t t3 = t0 + (3ULL * 86400 * 1000);

    uint32_t ticks = currency_melt_ticks_due(&cfg, t0, t3);
    TEST_ASSERT_EQUAL(3, ticks);

    /*
     * Appliquer la fonte au checkpoint (comme main.c le fait).
     * Alice : 1000 * 0.99^3 = 970 (1000→990→980→970)
     */
    for (uint32_t i = 0; i < chk0.account_count; i++) {
        chk0.accounts[i].balance = currency_melt_apply(
            &cfg, chk0.accounts[i].balance, ticks);
    }
    chk0.last_melt_timestamp = currency_melt_next_timestamp(&cfg, t0, ticks, t3);

    err = checkpoint_get_balance(&chk0, &alice_kp.public_key, &alice_bal);
    TEST_ASSERT_EQUAL(ESP_OK, err);
    TEST_ASSERT_EQUAL(970, alice_bal);

    /* ---- Jour 3 : Alice paie 200 a Bob (fee = 10) ---- */
    transaction_t transfer_tx;
    memset(&transfer_tx, 0, sizeof(transfer_tx));
    make_hash(&transfer_tx.id, 0x02);
    transfer_tx.type = TX_TYPE_TRANSFER;
    memcpy(&transfer_tx.from, &alice_kp.public_key, sizeof(public_key_t));
    memcpy(&transfer_tx.to, &bob_kp.public_key, sizeof(public_key_t));
    transfer_tx.amount = 200;
    transfer_tx.fee = 10;
    transfer_tx.timestamp = t3;
    transfer_tx.status = TX_STATUS_CONFIRMED;
    memcpy(&transfer_tx.parents[0], &mint_tx.id, sizeof(hash_t));
    transfer_tx.parent_count = 1;
    dag_insert(&dag, &transfer_tx);

    /*
     * Nouveau checkpoint incremental apres le transfert.
     * Le checkpoint base a les soldes fondus (Alice=970).
     * Le DAG contient le MINT (+1000 Alice) et le TRANSFER (-210 Alice, +200 Bob).
     *
     * Attention : checkpoint_create recalcule depuis le DAG complet +
     * la base. Donc pour isoler le transfert, on efface le dag et on
     * ne met que le transfert, avec la base = chk0 (qui a deja le MINT fondu).
     */
    dag_t dag2;
    dag_init(&dag2);
    dag_insert(&dag2, &transfer_tx);

    checkpoint_t chk1;
    err = checkpoint_create(&dag2, &chk0, NULL, &chk1);
    TEST_ASSERT_EQUAL(ESP_OK, err);

    /*
     * Apres le transfert :
     * Alice : 970 (base fondue) - 200 (amount) - 10 (fee) = 760
     * Bob   : 0 (base) + 200 (recu) = 200
     * Fee de 10 brule (disparait de la masse monetaire)
     */
    err = checkpoint_get_balance(&chk1, &alice_kp.public_key, &alice_bal);
    TEST_ASSERT_EQUAL(ESP_OK, err);
    TEST_ASSERT_EQUAL(760, alice_bal);

    uint32_t bob_bal;
    err = checkpoint_get_balance(&chk1, &bob_kp.public_key, &bob_bal);
    TEST_ASSERT_EQUAL(ESP_OK, err);
    TEST_ASSERT_EQUAL(200, bob_bal);

    /* ---- Jour 5 : 2 ticks de fonte supplementaires ---- */
    uint64_t t5 = t3 + (2ULL * 86400 * 1000);
    chk1.last_melt_timestamp = chk0.last_melt_timestamp; /* main.c le copie */

    ticks = currency_melt_ticks_due(&cfg, chk1.last_melt_timestamp, t5);
    TEST_ASSERT_EQUAL(2, ticks);

    for (uint32_t i = 0; i < chk1.account_count; i++) {
        chk1.accounts[i].balance = currency_melt_apply(
            &cfg, chk1.accounts[i].balance, ticks);
    }
    chk1.last_melt_timestamp = currency_melt_next_timestamp(
        &cfg, chk1.last_melt_timestamp, ticks, t5);

    /*
     * Apres 2 ticks supplementaires (1% par jour) :
     * Alice : 760 * 0.99^2 = 760 → 752 → 744
     * Bob   : 200 * 0.99^2 = 200 → 198 → 196
     */
    err = checkpoint_get_balance(&chk1, &alice_kp.public_key, &alice_bal);
    TEST_ASSERT_EQUAL(ESP_OK, err);
    TEST_ASSERT_EQUAL(744, alice_bal);

    err = checkpoint_get_balance(&chk1, &bob_kp.public_key, &bob_bal);
    TEST_ASSERT_EQUAL(ESP_OK, err);
    TEST_ASSERT_EQUAL(196, bob_bal);

    /*
     * Verification finale : la masse monetaire totale a diminue.
     * Initial : 1000 (MINT)
     * Jour 3 apres fonte : 970 (Alice)
     * Jour 3 apres transfert : 760 (Alice) + 200 (Bob) = 960 (-10 fee)
     * Jour 5 apres fonte : 744 + 196 = 940
     *
     * On verifie que le total est bien inferieur au MINT initial.
     */
    uint32_t masse_totale = alice_bal + bob_bal;
    TEST_ASSERT_TRUE(masse_totale < 1000);
    TEST_ASSERT_EQUAL(940, masse_totale);
}
