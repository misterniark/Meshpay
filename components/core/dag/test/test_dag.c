/**
 * @file test_dag.c
 * @brief Tests unitaires pour le module core/dag/.
 *
 * Teste l'insertion, la recherche, les tips, la validation contextuelle,
 * l'élagage et la fusion de sous-graphes.
 */

#include "unity.h"
#include "dag/dag.h"
#include "dag/dag_validate.h"
#include "dag/dag_prune.h"
#include "dag/dag_merge.h"
#include "transaction/tx_create.h"
#include "transaction/tx_validate.h"
#include "crypto/crypto_keys.h"
#include <string.h>

/* ========================================================================= */
/*                         Helpers de test                                    */
/* ========================================================================= */

/** Crée un hash fictif rempli d'un octet donné */
static void make_hash(hash_t *h, uint8_t fill)
{
    memset(h->bytes, fill, CRYPTO_HASH_SIZE);
}

/**
 * @brief Crée une transaction MINT simple pour peupler le DAG dans les tests.
 *
 * Utilise un hash fictif comme parent et un timestamp donné.
 */
static esp_err_t create_test_mint(transaction_t *tx, keypair_t *master,
                                   keypair_t *user, uint32_t amount,
                                   const hash_t *parent, uint64_t timestamp)
{
    return tx_create_mint(tx, master, &user->public_key,
                          amount, 0, 0, parent, 1, timestamp);
}

/* ========================================================================= */
/*                         Tests dag (base)                                   */
/* ========================================================================= */

/**
 * @brief Vérifie l'initialisation d'un DAG vide.
 */
TEST_CASE("dag_init_vide", "[dag]")
{
    dag_t dag;
    esp_err_t err = dag_init(&dag);

    TEST_ASSERT_EQUAL(ESP_OK, err);
    TEST_ASSERT_EQUAL(0, dag_count(&dag));
}

/**
 * @brief Vérifie l'insertion et la recherche par id.
 */
TEST_CASE("dag_insert_et_recherche", "[dag]")
{
    dag_t dag;
    dag_init(&dag);

    keypair_t master, user;
    crypto_generate_keypair(&master);
    crypto_generate_keypair(&user);

    hash_t parent;
    make_hash(&parent, 0xAA);

    transaction_t tx;
    create_test_mint(&tx, &master, &user, 100, &parent, 1000);

    /* Insérer */
    esp_err_t err = dag_insert(&dag, &tx);
    TEST_ASSERT_EQUAL(ESP_OK, err);
    TEST_ASSERT_EQUAL(1, dag_count(&dag));

    /* Rechercher par id */
    const transaction_t *found = dag_get_by_id(&dag, &tx.id);
    TEST_ASSERT_NOT_NULL(found);
    TEST_ASSERT_TRUE(hash_equal(&found->id, &tx.id));
    TEST_ASSERT_EQUAL(100, found->amount);
}

/**
 * @brief Vérifie le rejet d'un doublon.
 */
TEST_CASE("dag_insert_doublon", "[dag]")
{
    dag_t dag;
    dag_init(&dag);

    keypair_t master, user;
    crypto_generate_keypair(&master);
    crypto_generate_keypair(&user);

    hash_t parent;
    make_hash(&parent, 0xBB);

    transaction_t tx;
    create_test_mint(&tx, &master, &user, 200, &parent, 2000);

    dag_insert(&dag, &tx);

    /* Deuxième insertion du même tx → erreur */
    esp_err_t err = dag_insert(&dag, &tx);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, err);
    TEST_ASSERT_EQUAL(1, dag_count(&dag));
}

/**
 * @brief Vérifie la recherche d'un id inexistant.
 */
TEST_CASE("dag_recherche_inexistant", "[dag]")
{
    dag_t dag;
    dag_init(&dag);

    hash_t id;
    make_hash(&id, 0xFF);

    TEST_ASSERT_NULL(dag_get_by_id(&dag, &id));
    TEST_ASSERT_FALSE(dag_contains(&dag, &id));
}

/**
 * @brief Vérifie le calcul des tips.
 *
 * Scénario : TX-A (pas de parent dans le DAG) → TX-B (parent: TX-A)
 * Résultat attendu : seule TX-B est un tip (TX-A est référencée par TX-B)
 */
TEST_CASE("dag_tips", "[dag]")
{
    dag_t dag;
    dag_init(&dag);

    keypair_t master, user;
    crypto_generate_keypair(&master);
    crypto_generate_keypair(&user);

    /* TX-A : première transaction (parent fictif) */
    hash_t dummy_parent;
    make_hash(&dummy_parent, 0x01);

    transaction_t tx_a;
    create_test_mint(&tx_a, &master, &user, 100, &dummy_parent, 1000);
    dag_insert(&dag, &tx_a);

    /* TX-B : référence TX-A comme parent */
    transaction_t tx_b;
    tx_create_mint(&tx_b, &master, &user.public_key, 200, 0, 0, &tx_a.id, 1, 2000);
    dag_insert(&dag, &tx_b);

    /* Calculer les tips */
    const transaction_t *tips[DAG_MAX_TIPS];
    uint32_t tip_count = 0;
    dag_get_tips(&dag, tips, DAG_MAX_TIPS, &tip_count);

    /* Seul TX-B doit être un tip (TX-A est parent de TX-B) */
    TEST_ASSERT_EQUAL(1, tip_count);
    TEST_ASSERT_TRUE(hash_equal(&tips[0]->id, &tx_b.id));
}

/* ========================================================================= */
/*                         Tests dag_validate                                 */
/* ========================================================================= */

/**
 * @brief Vérifie la validation d'une TX dont les parents existent.
 */
TEST_CASE("dag_validate_parents_existants", "[dag]")
{
    dag_t dag;
    dag_init(&dag);

    keypair_t master, user;
    crypto_generate_keypair(&master);
    crypto_generate_keypair(&user);

    /* Insérer TX-A */
    hash_t dummy;
    make_hash(&dummy, 0x01);
    transaction_t tx_a;
    create_test_mint(&tx_a, &master, &user, 100, &dummy, 1000);
    dag_insert(&dag, &tx_a);

    /* TX-B référence TX-A → doit être valide */
    transaction_t tx_b;
    tx_create_mint(&tx_b, &master, &user.public_key, 200, 0, 0, &tx_a.id, 1, 2000);

    TEST_ASSERT_EQUAL(ESP_OK, dag_validate_transaction(&dag, &tx_b));
}

/**
 * @brief Vérifie le rejet d'une TX dont un parent n'existe pas dans le DAG.
 */
TEST_CASE("dag_validate_parent_manquant", "[dag]")
{
    dag_t dag;
    dag_init(&dag);

    keypair_t master, user;
    crypto_generate_keypair(&master);
    crypto_generate_keypair(&user);

    /* Parent fictif qui n'est pas dans le DAG */
    hash_t missing_parent;
    make_hash(&missing_parent, 0xFF);

    transaction_t tx;
    create_test_mint(&tx, &master, &user, 100, &missing_parent, 1000);

    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND, dag_validate_transaction(&dag, &tx));
}

/* ========================================================================= */
/*                         Tests dag_prune                                    */
/* ========================================================================= */

/**
 * @brief Vérifie l'élagage par timestamp.
 */
TEST_CASE("dag_prune_before_timestamp", "[dag]")
{
    dag_t dag;
    dag_init(&dag);

    keypair_t master, user;
    crypto_generate_keypair(&master);
    crypto_generate_keypair(&user);

    hash_t dummy;
    make_hash(&dummy, 0x01);

    /* Insérer 3 TX avec des timestamps croissants */
    transaction_t tx1, tx2, tx3;
    create_test_mint(&tx1, &master, &user, 100, &dummy, 1000);
    dag_insert(&dag, &tx1);

    create_test_mint(&tx2, &master, &user, 200, &tx1.id, 2000);
    dag_insert(&dag, &tx2);

    create_test_mint(&tx3, &master, &user, 300, &tx2.id, 3000);
    dag_insert(&dag, &tx3);

    TEST_ASSERT_EQUAL(3, dag_count(&dag));

    /* Élaguer tout ce qui est <= 2000 → ne garder que TX3 */
    dag_prune_before(&dag, 2000);

    TEST_ASSERT_EQUAL(1, dag_count(&dag));
    TEST_ASSERT_TRUE(dag_contains(&dag, &tx3.id));
    TEST_ASSERT_FALSE(dag_contains(&dag, &tx1.id));
    TEST_ASSERT_FALSE(dag_contains(&dag, &tx2.id));
}

/**
 * @brief Vérifie la détection du besoin de checkpoint.
 */
TEST_CASE("dag_needs_checkpoint_seuil", "[dag]")
{
    dag_t dag;
    dag_init(&dag);

    /* DAG vide → pas de checkpoint nécessaire */
    TEST_ASSERT_FALSE(dag_needs_checkpoint(&dag));

    /*
     * Le seuil est à 80% de DAG_MAX_TRANSACTIONS (soit 200 pour 250).
     * En dessous du seuil → pas de checkpoint.
     */
    dag.count = (DAG_MAX_TRANSACTIONS * 4 / 5) - 1;
    TEST_ASSERT_FALSE(dag_needs_checkpoint(&dag));

    /* Au seuil exact → checkpoint nécessaire */
    dag.count = DAG_MAX_TRANSACTIONS * 4 / 5;
    TEST_ASSERT_TRUE(dag_needs_checkpoint(&dag));

    /* Au-dessus du seuil → checkpoint toujours nécessaire */
    dag.count = DAG_MAX_TRANSACTIONS;
    TEST_ASSERT_TRUE(dag_needs_checkpoint(&dag));
}

/**
 * @brief Vérifie la purge complète du DAG.
 */
TEST_CASE("dag_prune_all", "[dag]")
{
    dag_t dag;
    dag_init(&dag);

    keypair_t master, user;
    crypto_generate_keypair(&master);
    crypto_generate_keypair(&user);

    hash_t dummy;
    make_hash(&dummy, 0x01);

    transaction_t tx;
    create_test_mint(&tx, &master, &user, 100, &dummy, 1000);
    dag_insert(&dag, &tx);

    TEST_ASSERT_EQUAL(1, dag_count(&dag));

    dag_prune_all(&dag);

    TEST_ASSERT_EQUAL(0, dag_count(&dag));
}

/* ========================================================================= */
/*                         Tests dag_merge                                    */
/* ========================================================================= */

/**
 * @brief Vérifie la fusion d'une transaction nouvelle.
 */
TEST_CASE("dag_merge_insert", "[dag]")
{
    dag_t dag;
    dag_init(&dag);

    keypair_t master, user;
    crypto_generate_keypair(&master);
    crypto_generate_keypair(&user);

    hash_t dummy;
    make_hash(&dummy, 0x01);

    transaction_t tx;
    create_test_mint(&tx, &master, &user, 100, &dummy, 1000);

    /* Construire la liste des clés maîtres pour la validation */
    const public_key_t master_key_list[] = { master.public_key };
    master_keys_t master_keys = { .keys = master_key_list, .count = 1 };

    dag_merge_result_t result;
    esp_err_t err = dag_merge_transaction(&dag, &tx, &master_keys, &result);

    TEST_ASSERT_EQUAL(ESP_OK, err);
    TEST_ASSERT_EQUAL(DAG_MERGE_INSERTED, result);
    TEST_ASSERT_EQUAL(1, dag_count(&dag));
}

/**
 * @brief Vérifie que la fusion ignore les doublons.
 */
TEST_CASE("dag_merge_doublon", "[dag]")
{
    dag_t dag;
    dag_init(&dag);

    keypair_t master, user;
    crypto_generate_keypair(&master);
    crypto_generate_keypair(&user);

    hash_t dummy;
    make_hash(&dummy, 0x01);

    transaction_t tx;
    create_test_mint(&tx, &master, &user, 100, &dummy, 1000);

    /* Construire la liste des clés maîtres pour la validation */
    const public_key_t master_key_list[] = { master.public_key };
    master_keys_t master_keys = { .keys = master_key_list, .count = 1 };

    dag_merge_result_t result;
    dag_merge_transaction(&dag, &tx, &master_keys, &result);

    /* Deuxième merge de la même TX → DUPLICATE */
    dag_merge_transaction(&dag, &tx, &master_keys, &result);
    TEST_ASSERT_EQUAL(DAG_MERGE_DUPLICATE, result);
    TEST_ASSERT_EQUAL(1, dag_count(&dag));
}

/**
 * @brief Vérifie la fusion par lot.
 */
TEST_CASE("dag_merge_batch_test", "[dag]")
{
    dag_t dag;
    dag_init(&dag);

    keypair_t master, user;
    crypto_generate_keypair(&master);
    crypto_generate_keypair(&user);

    hash_t dummy;
    make_hash(&dummy, 0x01);

    /* Créer 3 transactions */
    transaction_t batch[3];
    create_test_mint(&batch[0], &master, &user, 100, &dummy, 1000);
    create_test_mint(&batch[1], &master, &user, 200, &batch[0].id, 2000);
    create_test_mint(&batch[2], &master, &user, 300, &batch[1].id, 3000);

    /* Construire la liste des clés maîtres pour la validation */
    const public_key_t master_key_list[] = { master.public_key };
    master_keys_t master_keys = { .keys = master_key_list, .count = 1 };

    uint32_t inserted = 0;
    esp_err_t err = dag_merge_batch(&dag, batch, 3, &master_keys, &inserted);

    TEST_ASSERT_EQUAL(ESP_OK, err);
    TEST_ASSERT_EQUAL(3, inserted);
    TEST_ASSERT_EQUAL(3, dag_count(&dag));
}

/**
 * @brief Vérifie que la fusion accepte les TX avec parents manquants.
 *
 * C'est une différence clé avec dag_validate : lors de la sync LoRa,
 * les parents peuvent arriver dans un paquet ultérieur.
 */
TEST_CASE("dag_merge_parents_manquants", "[dag]")
{
    dag_t dag;
    dag_init(&dag);

    keypair_t master, user;
    crypto_generate_keypair(&master);
    crypto_generate_keypair(&user);

    /* Parent fictif qui n'est pas dans le DAG */
    hash_t missing_parent;
    make_hash(&missing_parent, 0xEE);

    transaction_t tx;
    create_test_mint(&tx, &master, &user, 100, &missing_parent, 1000);

    /* Construire la liste des clés maîtres pour la validation */
    const public_key_t master_key_list[] = { master.public_key };
    master_keys_t master_keys = { .keys = master_key_list, .count = 1 };

    /* La fusion doit accepter même sans les parents */
    dag_merge_result_t result;
    esp_err_t err = dag_merge_transaction(&dag, &tx, &master_keys, &result);

    TEST_ASSERT_EQUAL(ESP_OK, err);
    TEST_ASSERT_EQUAL(DAG_MERGE_INSERTED, result);
}

/**
 * @brief [I3-fix] Verifie la detection de conflit par (from, seq).
 *
 * Deux TX MINT distinctes (timestamps et amounts differents → id
 * different) emises par le MEME master avec le MEME seq doivent etre
 * detectees comme un conflit. La premiere est acceptee, la deuxieme
 * est rejetee avec DAG_MERGE_CONFLICT.
 */
TEST_CASE("dag_merge_conflit_seq_meme_from", "[dag]")
{
    dag_t dag;
    dag_init(&dag);

    keypair_t master, user;
    crypto_generate_keypair(&master);
    crypto_generate_keypair(&user);

    hash_t parent;
    make_hash(&parent, 0x42);

    const public_key_t master_key_list[] = { master.public_key };
    master_keys_t master_keys = { .keys = master_key_list, .count = 1 };

    /* Premiere TX : seq=5, amount=100, ts=1000 → INSERTED */
    transaction_t tx1;
    TEST_ASSERT_EQUAL(ESP_OK,
        tx_create_mint(&tx1, &master, &user.public_key,
                       100, 0, 5, &parent, 1, 1000));
    dag_merge_result_t r1;
    TEST_ASSERT_EQUAL(ESP_OK, dag_merge_transaction(&dag, &tx1, &master_keys, &r1));
    TEST_ASSERT_EQUAL(DAG_MERGE_INSERTED, r1);

    /* Deuxieme TX : MEME seq=5 mais amount/ts differents → id different → CONFLICT */
    transaction_t tx2;
    TEST_ASSERT_EQUAL(ESP_OK,
        tx_create_mint(&tx2, &master, &user.public_key,
                       999, 0, 5, &parent, 1, 2000));
    dag_merge_result_t r2;
    TEST_ASSERT_EQUAL(ESP_OK, dag_merge_transaction(&dag, &tx2, &master_keys, &r2));
    TEST_ASSERT_EQUAL(DAG_MERGE_CONFLICT, r2);

    /* Le DAG ne doit contenir que la premiere TX */
    TEST_ASSERT_EQUAL(1, dag_count(&dag));
}

/**
 * @brief [I3-fix] Meme seq mais emetteurs differents → pas de conflit.
 *
 * Le seq est scope par emetteur : deux devices peuvent avoir le meme
 * seq sans conflit. Cela garantit que chaque device maintient
 * uniquement son propre compteur.
 */
TEST_CASE("dag_merge_meme_seq_emetteurs_differents", "[dag]")
{
    dag_t dag;
    dag_init(&dag);

    keypair_t master_a, master_b, user;
    crypto_generate_keypair(&master_a);
    crypto_generate_keypair(&master_b);
    crypto_generate_keypair(&user);

    hash_t parent;
    make_hash(&parent, 0x33);

    /* master_a ET master_b sont tous deux autorises */
    const public_key_t master_key_list[] = { master_a.public_key, master_b.public_key };
    master_keys_t master_keys = { .keys = master_key_list, .count = 2 };

    /* Master A : seq=1 */
    transaction_t tx_a;
    TEST_ASSERT_EQUAL(ESP_OK,
        tx_create_mint(&tx_a, &master_a, &user.public_key,
                       100, 0, 1, &parent, 1, 1000));
    dag_merge_result_t r_a;
    dag_merge_transaction(&dag, &tx_a, &master_keys, &r_a);
    TEST_ASSERT_EQUAL(DAG_MERGE_INSERTED, r_a);

    /* Master B : MEME seq=1 → pas de conflit car from different */
    transaction_t tx_b;
    TEST_ASSERT_EQUAL(ESP_OK,
        tx_create_mint(&tx_b, &master_b, &user.public_key,
                       200, 0, 1, &parent, 1, 2000));
    dag_merge_result_t r_b;
    dag_merge_transaction(&dag, &tx_b, &master_keys, &r_b);
    TEST_ASSERT_EQUAL(DAG_MERGE_INSERTED, r_b);

    TEST_ASSERT_EQUAL(2, dag_count(&dag));
}
