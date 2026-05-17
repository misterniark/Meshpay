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
#include <stdlib.h>

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
 *
 * [F-DG-026] LIMITATION : ce helper hardcode `seq=0`. Plusieurs MINT
 * créés depuis le même `master` auront tous le même `seq` et seraient
 * rejetés par `dag_merge_transaction` (DAG_MERGE_CONFLICT) car le
 * détecteur de double-dépense (I3-fix) repose sur l'unicité du couple
 * (from, seq). Pour les tests d'insertion directe via `dag_insert`
 * cela ne pose pas de problème (dag_insert ne vérifie pas seq), mais
 * tout test impliquant le pipeline merge doit appeler `tx_create_mint`
 * directement avec des seq distincts (cf. `dag_merge_batch_test` qui
 * utilise seq=1, 2, 3 explicitement), ou utiliser
 * create_test_mint_seq ci-dessous.
 */
static esp_err_t create_test_mint(transaction_t *tx, keypair_t *master,
                                   keypair_t *user, uint32_t amount,
                                   const hash_t *parent, uint64_t timestamp)
{
    return tx_create_mint(tx, master, &user->public_key,
                          amount, 0, 0, parent, 1, timestamp);
}

/**
 * [F-DG-026] Variante paramétrée par `seq`, utilisable pour tester
 * des scénarios multi-MINT du même émetteur via le pipeline merge.
 *
 * Le `__attribute__((unused))` documente que ce helper est fourni
 * comme outil partagé pour les futurs tests d'intégration merge,
 * sans être encore utilisé par les tests existants (qui appellent
 * `tx_create_mint` directement ou passent par `create_test_mint`
 * avec insertion via `dag_insert` qui ne valide pas le seq).
 */
static __attribute__((unused))
esp_err_t create_test_mint_seq(transaction_t *tx, keypair_t *master,
                                keypair_t *user, uint32_t amount,
                                uint32_t seq,
                                const hash_t *parent, uint64_t timestamp)
{
    return tx_create_mint(tx, master, &user->public_key,
                          amount, 0, seq, parent, 1, timestamp);
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

    /* Lot E.1bis : le helper `create_test_mint` passe `seq=0` pour
     * toutes les TX du meme emetteur, ce qui declenche la detection
     * de conflit nonce monotone introduite au Lot B (I3-fix). On
     * appelle tx_create_mint directement avec des seq distincts
     * (1, 2, 3) pour simuler un emetteur respectant son compteur. */
    transaction_t batch[3];
    tx_create_mint(&batch[0], &master, &user.public_key, 100, 0, 1, &dummy,         1, 1000);
    tx_create_mint(&batch[1], &master, &user.public_key, 200, 0, 2, &batch[0].id,   1, 2000);
    tx_create_mint(&batch[2], &master, &user.public_key, 300, 0, 3, &batch[1].id,   1, 3000);

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
 * detectees comme un conflit. Elles sont conservees toutes les deux,
 * mais une seule reste comptable : le plus petit tx_id gagne, l'autre
 * est marquee CANCELLED. Cette regle remplace "first seen wins" par
 * une convergence deterministe.
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

    TEST_ASSERT_EQUAL(2, dag_count(&dag));
    const transaction_t *stored1 = dag_get_by_id(&dag, &tx1.id);
    const transaction_t *stored2 = dag_get_by_id(&dag, &tx2.id);
    TEST_ASSERT_NOT_NULL(stored1);
    TEST_ASSERT_NOT_NULL(stored2);

    const bool tx1_wins = memcmp(tx1.id.bytes, tx2.id.bytes,
                                 CRYPTO_HASH_SIZE) < 0;
    TEST_ASSERT_EQUAL(tx1_wins ? TX_STATUS_CONFIRMED : TX_STATUS_CANCELLED,
                      stored1->status);
    TEST_ASSERT_EQUAL(tx1_wins ? TX_STATUS_CANCELLED : TX_STATUS_CONFIRMED,
                      stored2->status);
}

TEST_CASE("dag_merge_conflit_seq_converge_meme_si_ordre_inverse", "[dag]")
{
    dag_t *dag_ab = (dag_t *)malloc(sizeof(*dag_ab));
    dag_t *dag_ba = (dag_t *)malloc(sizeof(*dag_ba));
    TEST_ASSERT_NOT_NULL(dag_ab);
    TEST_ASSERT_NOT_NULL(dag_ba);
    dag_init(dag_ab);
    dag_init(dag_ba);

    keypair_t master, user;
    crypto_generate_keypair(&master);
    crypto_generate_keypair(&user);

    hash_t parent;
    make_hash(&parent, 0x55);

    const public_key_t master_key_list[] = { master.public_key };
    master_keys_t master_keys = { .keys = master_key_list, .count = 1 };

    transaction_t tx_a;
    transaction_t tx_b;
    TEST_ASSERT_EQUAL(ESP_OK,
        tx_create_mint(&tx_a, &master, &user.public_key,
                       100, 0, 8, &parent, 1, 1000));
    TEST_ASSERT_EQUAL(ESP_OK,
        tx_create_mint(&tx_b, &master, &user.public_key,
                       200, 0, 8, &parent, 1, 2000));

    dag_merge_result_t r;
    TEST_ASSERT_EQUAL(ESP_OK, dag_merge_transaction(dag_ab, &tx_a, &master_keys, &r));
    TEST_ASSERT_EQUAL(ESP_OK, dag_merge_transaction(dag_ab, &tx_b, &master_keys, &r));
    TEST_ASSERT_EQUAL(ESP_OK, dag_merge_transaction(dag_ba, &tx_b, &master_keys, &r));
    TEST_ASSERT_EQUAL(ESP_OK, dag_merge_transaction(dag_ba, &tx_a, &master_keys, &r));

    const bool a_wins = memcmp(tx_a.id.bytes, tx_b.id.bytes,
                               CRYPTO_HASH_SIZE) < 0;
    const transaction_t *ab_a = dag_get_by_id(dag_ab, &tx_a.id);
    const transaction_t *ab_b = dag_get_by_id(dag_ab, &tx_b.id);
    const transaction_t *ba_a = dag_get_by_id(dag_ba, &tx_a.id);
    const transaction_t *ba_b = dag_get_by_id(dag_ba, &tx_b.id);
    TEST_ASSERT_NOT_NULL(ab_a);
    TEST_ASSERT_NOT_NULL(ab_b);
    TEST_ASSERT_NOT_NULL(ba_a);
    TEST_ASSERT_NOT_NULL(ba_b);

    TEST_ASSERT_EQUAL(a_wins ? TX_STATUS_CONFIRMED : TX_STATUS_CANCELLED,
                      ab_a->status);
    TEST_ASSERT_EQUAL(a_wins ? TX_STATUS_CANCELLED : TX_STATUS_CONFIRMED,
                      ab_b->status);
    TEST_ASSERT_EQUAL(ab_a->status, ba_a->status);
    TEST_ASSERT_EQUAL(ab_b->status, ba_b->status);

    free(dag_ab);
    free(dag_ba);
}

TEST_CASE("dag_tips_ignore_branches_cancelled_by_conflict_resolution", "[dag]")
{
    dag_t dag;
    dag_init(&dag);

    keypair_t master, user;
    crypto_generate_keypair(&master);
    crypto_generate_keypair(&user);

    hash_t parent;
    make_hash(&parent, 0x66);

    const public_key_t master_key_list[] = { master.public_key };
    master_keys_t master_keys = { .keys = master_key_list, .count = 1 };

    transaction_t tx_a;
    transaction_t tx_b;
    TEST_ASSERT_EQUAL(ESP_OK,
        tx_create_mint(&tx_a, &master, &user.public_key,
                       100, 0, 9, &parent, 1, 1000));
    TEST_ASSERT_EQUAL(ESP_OK,
        tx_create_mint(&tx_b, &master, &user.public_key,
                       200, 0, 9, &parent, 1, 2000));

    dag_merge_result_t r;
    TEST_ASSERT_EQUAL(ESP_OK, dag_merge_transaction(&dag, &tx_a, &master_keys, &r));
    TEST_ASSERT_EQUAL(ESP_OK, dag_merge_transaction(&dag, &tx_b, &master_keys, &r));

    const transaction_t *tips[2] = {0};
    uint32_t tip_count = 0;
    TEST_ASSERT_EQUAL(ESP_OK, dag_get_tips(&dag, tips, 2, &tip_count));
    TEST_ASSERT_EQUAL_UINT32(1, tip_count);
    TEST_ASSERT_NOT_NULL(tips[0]);
    TEST_ASSERT_NOT_EQUAL(TX_STATUS_CANCELLED, tips[0]->status);
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

/* ========================================================================= */
/*           Tests ajoutes par l'audit 2026-05-15 (F-DG-027/028/029)         */
/* ========================================================================= */

/**
 * [F-DG-027] Verifie que dag_set_status rejette les transitions
 * interdites depuis un etat terminal.
 *
 * Cycle de vie attendu :
 *   LOCKED   → CONFIRMED  : autorise
 *   LOCKED   → CANCELLED  : autorise
 *   CONFIRMED → quoi que ce soit : interdit (terminal)
 *   CANCELLED → quoi que ce soit : interdit (terminal)
 *   Same-status : idempotent (ESP_OK silencieux)
 */
TEST_CASE("dag_set_status_transitions_invalides_rejetees", "[dag]")
{
    dag_t dag;
    TEST_ASSERT_EQUAL(ESP_OK, dag_init(&dag));

    keypair_t master, user;
    TEST_ASSERT_EQUAL(ESP_OK, crypto_generate_keypair(&master));
    TEST_ASSERT_EQUAL(ESP_OK, crypto_generate_keypair(&user));
    hash_t parent;
    memset(&parent, 0xAB, sizeof(parent));

    /* Inserer une TX LOCKED (statut par defaut a la creation manuelle) */
    transaction_t tx;
    TEST_ASSERT_EQUAL(ESP_OK,
        create_test_mint(&tx, &master, &user, 100, &parent, 1000));
    tx.status = TX_STATUS_LOCKED;
    TEST_ASSERT_EQUAL(ESP_OK, dag_insert(&dag, &tx));

    /* LOCKED → CONFIRMED : autorise */
    TEST_ASSERT_EQUAL(ESP_OK,
        dag_set_status(&dag, &tx.id, TX_STATUS_CONFIRMED));

    /* CONFIRMED → CONFIRMED : idempotent */
    TEST_ASSERT_EQUAL(ESP_OK,
        dag_set_status(&dag, &tx.id, TX_STATUS_CONFIRMED));

    /* CONFIRMED → CANCELLED : INTERDIT (etat terminal) */
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE,
        dag_set_status(&dag, &tx.id, TX_STATUS_CANCELLED));

    /* CONFIRMED → LOCKED : INTERDIT (etat terminal) */
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE,
        dag_set_status(&dag, &tx.id, TX_STATUS_LOCKED));

    /* Hash inconnu : NOT_FOUND */
    hash_t unknown;
    memset(&unknown, 0xFF, sizeof(unknown));
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND,
        dag_set_status(&dag, &unknown, TX_STATUS_CONFIRMED));
}

/**
 * [F-DG-028] Verifie que dag_insert rejette la TX qui sature le DAG.
 *
 * On remplit le DAG jusqu'a DAG_MAX_TRANSACTIONS avec des TX distinctes
 * (parents et seq differents) puis on tente d'inserer une TX de plus
 * — doit retourner ESP_ERR_NO_MEM sans crash.
 *
 * Note : on alloue le dag_t en static pour eviter l'overflow de pile
 * (sizeof(dag_t) ≈ 58 Ko, depasse la pile par defaut des taches Unity).
 */
TEST_CASE("dag_insert_saturation_rejette_la_251eme", "[dag]")
{
    static dag_t dag;
    TEST_ASSERT_EQUAL(ESP_OK, dag_init(&dag));

    keypair_t master, user;
    TEST_ASSERT_EQUAL(ESP_OK, crypto_generate_keypair(&master));
    TEST_ASSERT_EQUAL(ESP_OK, crypto_generate_keypair(&user));
    hash_t parent;
    memset(&parent, 0xAB, sizeof(parent));

    /* Remplir le DAG (DAG_MAX_TRANSACTIONS = 250).
     * On utilise des timestamps croissants pour avoir des hashes
     * distincts (le hash inclut le timestamp via tx_serialize). */
    for (uint32_t i = 0; i < DAG_MAX_TRANSACTIONS; i++) {
        transaction_t tx;
        TEST_ASSERT_EQUAL(ESP_OK,
            create_test_mint(&tx, &master, &user,
                              (uint32_t)(100 + i), &parent,
                              (uint64_t)(1000 + i)));
        TEST_ASSERT_EQUAL(ESP_OK, dag_insert(&dag, &tx));
    }

    TEST_ASSERT_EQUAL(DAG_MAX_TRANSACTIONS, dag_count(&dag));

    /* TX 251 : doit etre rejetee */
    transaction_t overflow_tx;
    TEST_ASSERT_EQUAL(ESP_OK,
        create_test_mint(&overflow_tx, &master, &user, 9999, &parent,
                          (uint64_t)(1000 + DAG_MAX_TRANSACTIONS)));
    TEST_ASSERT_EQUAL(ESP_ERR_NO_MEM, dag_insert(&dag, &overflow_tx));

    /* Le DAG est toujours a la limite, pas overflow */
    TEST_ASSERT_EQUAL(DAG_MAX_TRANSACTIONS, dag_count(&dag));
}

/**
 * [F-DG-005 / F-DG-029] Verifie que dag_prune_before NE supprime PAS
 * les TX LOCKED, meme si leur timestamp est ancien.
 *
 * Scenario : 3 TX inserees avec timestamps 100, 200, 300.
 * - TX 100 est CONFIRMED → doit etre prunee
 * - TX 200 est LOCKED    → doit etre conservee malgre son timestamp ancien
 * - TX 300 est CONFIRMED → doit etre conservee (timestamp > seuil)
 *
 * Apres prune avant timestamp 250 :
 *   - dag_count() == 2 (TX 200 LOCKED + TX 300 CONFIRMED)
 *   - dag_get_by_id sur TX 200 retourne non-NULL
 */
TEST_CASE("dag_prune_preserve_locked_meme_si_ancienne", "[dag]")
{
    dag_t dag;
    TEST_ASSERT_EQUAL(ESP_OK, dag_init(&dag));

    keypair_t master, user;
    TEST_ASSERT_EQUAL(ESP_OK, crypto_generate_keypair(&master));
    TEST_ASSERT_EQUAL(ESP_OK, crypto_generate_keypair(&user));
    hash_t parent;
    memset(&parent, 0xAB, sizeof(parent));

    transaction_t tx_confirmed_old, tx_locked_old, tx_confirmed_new;
    TEST_ASSERT_EQUAL(ESP_OK,
        create_test_mint(&tx_confirmed_old, &master, &user, 10, &parent, 100));
    tx_confirmed_old.status = TX_STATUS_CONFIRMED;

    TEST_ASSERT_EQUAL(ESP_OK,
        create_test_mint(&tx_locked_old, &master, &user, 20, &parent, 200));
    tx_locked_old.status = TX_STATUS_LOCKED;

    TEST_ASSERT_EQUAL(ESP_OK,
        create_test_mint(&tx_confirmed_new, &master, &user, 30, &parent, 300));
    tx_confirmed_new.status = TX_STATUS_CONFIRMED;

    TEST_ASSERT_EQUAL(ESP_OK, dag_insert(&dag, &tx_confirmed_old));
    TEST_ASSERT_EQUAL(ESP_OK, dag_insert(&dag, &tx_locked_old));
    TEST_ASSERT_EQUAL(ESP_OK, dag_insert(&dag, &tx_confirmed_new));
    TEST_ASSERT_EQUAL(3, dag_count(&dag));

    /* Prune avant timestamp 250 :
     *   - 100 CONFIRMED → supprime
     *   - 200 LOCKED    → conserve (F-DG-005)
     *   - 300 CONFIRMED → conserve (timestamp > seuil)
     */
    TEST_ASSERT_EQUAL(ESP_OK, dag_prune_before(&dag, 250));
    TEST_ASSERT_EQUAL(2, dag_count(&dag));

    /* La LOCKED ancienne est toujours retrouvable */
    const transaction_t *still_locked =
        dag_get_by_id(&dag, &tx_locked_old.id);
    TEST_ASSERT_NOT_NULL(still_locked);
    TEST_ASSERT_EQUAL(TX_STATUS_LOCKED, still_locked->status);

    /* La CONFIRMED ancienne a bien disparu */
    TEST_ASSERT_NULL(dag_get_by_id(&dag, &tx_confirmed_old.id));
}
