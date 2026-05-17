/**
 * @file dag_merge.c
 * @brief Implémentation de la fusion de sous-graphes (sync LoRa).
 *
 * Quand un device reçoit des transactions via LoRa, ce module les
 * intègre dans le DAG local. Les doublons sont ignorés, les nouvelles
 * transactions sont insérées.
 *
 * Particularité : contrairement à dag_validate, la fusion accepte les
 * transactions dont les parents ne sont pas encore dans le DAG local.
 * Ces parents arriveront dans une synchronisation ultérieure (convergence
 * éventuelle).
 */

#include "dag/dag_merge.h"
#include "transaction/tx_validate.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include <assert.h>
#include <string.h>

static const char *TAG = "dag_merge";

static int hash_compare_lex(const hash_t *a, const hash_t *b)
{
    return memcmp(a->bytes, b->bytes, CRYPTO_HASH_SIZE);
}

static bool same_seq_scope(const transaction_t *a, const transaction_t *b)
{
    return a != NULL && b != NULL &&
           a->seq == b->seq &&
           public_key_equal(&a->from, &b->from);
}

esp_err_t dag_merge_transaction(dag_t *dag, const transaction_t *tx,
                                const master_keys_t *master_keys,
                                dag_merge_result_t *result)
{
    if (dag == NULL || tx == NULL || result == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /*
     * [F-DG-014] Validations cryptographiques HORS section critique.
     *
     * tx_validate_structure et tx_validate_signature n'accèdent pas
     * au DAG (elles ne lisent que `tx` lui-même), donc on peut les
     * exécuter sans tenir le mutex. tx_validate_signature recalcule
     * un hash SHA-256 et vérifie une signature Ed25519 — opération
     * lourde (~5 ms sur ESP32) qui bloquait précédemment toutes les
     * autres opérations DAG concurrentes. Avec ce déplacement, le
     * mutex n'est tenu que pendant les opérations rapides : doublon,
     * conflit seq, insertion.
     *
     * Conséquence : un mauvais comportement crypto (signature
     * invalide, structure malformée) est détecté tôt sans pénaliser
     * les autres threads.
     */

    /* Étape 1 : vérification structurelle (sans lock). */
    if (tx_validate_structure(tx) != ESP_OK) {
        /*
         * [F-DG-022] Loguer pourquoi le merge a echoue pour permettre
         * un diagnostic rapide en banc sans recompiler avec
         * ESP_LOGD active. Champs clefs uniquement (pas le pubkey
         * complet ni le hash : on ne veut pas spammer le log INFO
         * en cas de rejets en serie).
         */
        ESP_LOGW(TAG, "merge rejete (structure invalide): type=%d amount=%"PRIu32
                      " parent_count=%u from[0]=0x%02x to[0]=0x%02x",
                 (int)tx->type, tx->amount, (unsigned)tx->parent_count,
                 tx->from.bytes[0], tx->to.bytes[0]);
        *result = DAG_MERGE_REJECTED;
        return ESP_ERR_INVALID_ARG;
    }

    /* Étape 2 : vérification du hash et de la signature Ed25519 (sans lock). */
    if (tx_validate_signature(tx) != ESP_OK) {
        ESP_LOGW(TAG, "merge rejete (signature invalide): type=%d from[0]=0x%02x",
                 (int)tx->type, tx->from.bytes[0]);
        *result = DAG_MERGE_REJECTED;
        return ESP_ERR_INVALID_ARG;
    }

    /*
     * Étape 3 : pour les MINT, vérifier que le signataire est un maître
     * autorisé (sans lock).
     *
     * [F-DG-002] Si master_keys == NULL, on REJETTE le MINT au lieu de
     * l'accepter aveuglément. L'ancien comportement (skip de la vérif
     * d'autorité quand master_keys est NULL) ouvrait une surface
     * d'attaque : tout caller passant NULL — par commodité ou par bug
     * — désactivait silencieusement la vérification d'autorité, et un
     * attaquant LoRa pouvait injecter un MINT signé avec sa propre clé.
     * On garde la possibilité de passer NULL pour le code de test qui
     * n'a pas besoin de cette vérification (TRANSFER uniquement), mais
     * tout MINT reçu via cette voie est désormais rejeté explicitement.
     */
    if (tx->type == TX_TYPE_MINT) {
        if (master_keys == NULL) {
            ESP_LOGW(TAG, "merge rejete (MINT sans master_keys)");
            *result = DAG_MERGE_REJECTED;
            return ESP_ERR_INVALID_ARG;
        }
        if (tx_validate_master(tx, master_keys) != ESP_OK) {
            ESP_LOGW(TAG, "merge rejete (MINT non autorise): from[0]=0x%02x "
                          "master_count=%u",
                     tx->from.bytes[0], (unsigned)master_keys->count);
            *result = DAG_MERGE_REJECTED;
            return ESP_ERR_INVALID_ARG;
        }
    }

    /*
     * À partir d'ici on a besoin du DAG : doublon, conflit seq,
     * insertion. Section critique aussi courte que possible.
     */
    xSemaphoreTakeRecursive(dag->mutex, portMAX_DELAY);

    /* Si la transaction existe déjà → doublon, on ignore */
    if (dag_contains(dag, &tx->id)) {
        *result = DAG_MERGE_DUPLICATE;
        xSemaphoreGiveRecursive(dag->mutex);
        return ESP_OK;
    }

    /*
     * Convergence long terme : resolution deterministe des conflits
     * (from, seq).
     *
     * L'ancien modele etait "first seen wins" : deux devices recevant
     * deux branches valides dans des ordres differents pouvaient garder
     * des gagnants differents indefiniment. On utilise maintenant un
     * ordre canonique stable sur tx_id : pour un meme emetteur et un
     * meme seq, la TX dont le hash est lexicographiquement le plus petit
     * gagne. Les perdantes restent dans le DAG avec status CANCELLED pour
     * audit, dedup et propagation, mais le wallet ne les comptabilise pas.
     */
    bool has_conflict = false;
    bool incoming_wins = true;
    for (uint32_t i = 0; i < dag->count; i++) {
        transaction_t *existing = &dag->transactions[i];
        if (!same_seq_scope(existing, tx) ||
            hash_equal(&existing->id, &tx->id)) {
            continue;
        }

        has_conflict = true;
        if (hash_compare_lex(&existing->id, &tx->id) < 0) {
            incoming_wins = false;
        }
    }

    if (has_conflict) {
        if (dag->count >= DAG_MAX_TRANSACTIONS) {
            *result = DAG_MERGE_REJECTED;
            xSemaphoreGiveRecursive(dag->mutex);
            return ESP_ERR_NO_MEM;
        }

        transaction_t stored;
        memcpy(&stored, tx, sizeof(stored));

        if (incoming_wins) {
            for (uint32_t i = 0; i < dag->count; i++) {
                transaction_t *existing = &dag->transactions[i];
                if (same_seq_scope(existing, &stored) &&
                    !hash_equal(&existing->id, &stored.id)) {
                    existing->status = TX_STATUS_CANCELLED;
                }
            }
        } else {
            stored.status = TX_STATUS_CANCELLED;
        }

        assert(dag->count < DAG_MAX_TRANSACTIONS);
        memcpy(&dag->transactions[dag->count], &stored, sizeof(stored));
        dag->count++;

        *result = DAG_MERGE_CONFLICT;
        xSemaphoreGiveRecursive(dag->mutex);
        return ESP_OK;
    }

    /* Si le DAG est plein → rejeté */
    if (dag->count >= DAG_MAX_TRANSACTIONS) {
        *result = DAG_MERGE_REJECTED;
        xSemaphoreGiveRecursive(dag->mutex);
        return ESP_ERR_NO_MEM;
    }

    /*
     * Insérer la transaction (les parents manquants ne sont PAS bloquants).
     * [F-DG-010] Assert défensif sur la capacité : la vérification
     * ci-dessus garantit dag->count < DAG_MAX_TRANSACTIONS, mais on
     * laisse une trace explicite à l'usage du compactage manuel — si
     * un futur refactoring supprime ce check, l'assert sautera en
     * debug avant tout out-of-bounds.
     */
    assert(dag->count < DAG_MAX_TRANSACTIONS);
    memcpy(&dag->transactions[dag->count], tx, sizeof(transaction_t));
    dag->count++;

    *result = DAG_MERGE_INSERTED;
    xSemaphoreGiveRecursive(dag->mutex);
    return ESP_OK;
}

esp_err_t dag_merge_batch(dag_t *dag, const transaction_t *transactions,
                          uint32_t count, const master_keys_t *master_keys,
                          uint32_t *inserted_count)
{
    if (dag == NULL || transactions == NULL || inserted_count == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    *inserted_count = 0;

    for (uint32_t i = 0; i < count; i++) {
        dag_merge_result_t result;
        esp_err_t err = dag_merge_transaction(dag, &transactions[i],
                                              master_keys, &result);

        if (result == DAG_MERGE_INSERTED) {
            (*inserted_count)++;
        } else {
            /*
             * [F-DG-015] Tracer les TX du lot non insérées pour
             * faciliter le diagnostic. Avant ce fix, dag_merge_batch
             * ignorait silencieusement DUPLICATE / CONFLICT / REJECTED
             * et l'appelant ne pouvait que constater `inserted_count <
             * count` sans savoir pourquoi.
             */
            const char *reason = "?";
            switch (result) {
                case DAG_MERGE_DUPLICATE: reason = "duplicate"; break;
                case DAG_MERGE_CONFLICT:  reason = "seq-conflict"; break;
                case DAG_MERGE_REJECTED:  reason = "rejected"; break;
                default: break;
            }
            ESP_LOGW(TAG, "Batch TX %lu/%lu: %s (err=0x%x)",
                     (unsigned long)(i + 1), (unsigned long)count,
                     reason, err);
        }

        /* Si le DAG est plein, on arrête */
        if (err == ESP_ERR_NO_MEM) {
            return ESP_ERR_NO_MEM;
        }
    }

    return ESP_OK;
}
