/**
 * @file dag.c
 * @brief Implémentation du DAG (structure de base, insertion, recherche, tips).
 *
 * Le DAG est stocké dans un tableau statique de taille fixe
 * DAG_MAX_TRANSACTIONS (250 TX, voir dag.h pour la justification
 * mémoire). [F-DG-012] La valeur "500" qui apparaissait dans ce
 * commentaire avant 2026-05-15 était périmée.
 *
 * La recherche par hash est linéaire O(n), acceptable pour cette taille.
 *
 * Les "tips" sont calculées dynamiquement : on parcourt toutes les TX
 * et on identifie celles qui ne sont référencées par aucune autre TX
 * comme parent.
 */

#include "dag/dag.h"
#include <string.h>

esp_err_t dag_init(dag_t *dag)
{
    if (dag == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(dag, 0, sizeof(dag_t));

    /* Créer un mutex récursif pour protéger les accès concurrents au DAG.
     * Sur ESP32, plusieurs tâches FreeRTOS (LoRa RX, UI, wallet)
     * accèdent au DAG simultanément. Sans mutex, les écritures
     * concurrentes corrompent le tableau et le compteur.
     *
     * On utilise un mutex récursif car certaines fonctions publiques
     * (dag_insert, dag_set_status) appellent d'autres fonctions
     * publiques protégées (dag_contains, dag_get_by_id). */
    dag->mutex = xSemaphoreCreateRecursiveMutex();
    if (dag->mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

esp_err_t dag_insert(dag_t *dag, const transaction_t *tx)
{
    if (dag == NULL || tx == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTakeRecursive(dag->mutex, portMAX_DELAY);

    /* Vérifier que le DAG n'est pas plein */
    if (dag->count >= DAG_MAX_TRANSACTIONS) {
        xSemaphoreGiveRecursive(dag->mutex);
        return ESP_ERR_NO_MEM;
    }

    /* Vérifier que la transaction n'existe pas déjà (pas de doublon) */
    if (dag_contains(dag, &tx->id)) {
        xSemaphoreGiveRecursive(dag->mutex);
        return ESP_ERR_INVALID_STATE;
    }

    /* Copier la transaction dans le prochain slot disponible */
    memcpy(&dag->transactions[dag->count], tx, sizeof(transaction_t));
    dag->count++;

    xSemaphoreGiveRecursive(dag->mutex);
    return ESP_OK;
}

const transaction_t *dag_get_by_id(const dag_t *dag, const hash_t *id)
{
    if (dag == NULL || id == NULL) {
        return NULL;
    }

    xSemaphoreTakeRecursive(dag->mutex, portMAX_DELAY);

    /* Recherche linéaire O(n) */
    const transaction_t *found = NULL;
    for (uint32_t i = 0; i < dag->count; i++) {
        if (hash_equal(&dag->transactions[i].id, id)) {
            found = &dag->transactions[i];
            break;
        }
    }

    xSemaphoreGiveRecursive(dag->mutex);
    return found;
}

esp_err_t dag_get_tips_ext(const dag_t *dag, const transaction_t **tips,
                            uint32_t max_tips, uint32_t *tip_count,
                            uint32_t *total_tips_found)
{
    if (dag == NULL || tips == NULL || tip_count == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTakeRecursive(dag->mutex, portMAX_DELAY);

    *tip_count = 0;
    uint32_t total = 0;

    /*
     * Une transaction est un "tip" si aucune autre transaction dans le DAG
     * ne la référence comme parent.
     *
     * Algorithme O(n²) sur la détection ; insertion-sort en O(max_tips)
     * par tip trouvé pour maintenir l'ordre "plus récent en tête".
     *
     * [F-DG-018] Sélection par timestamp décroissant : on parcourt
     * toutes les TX, on identifie celles qui sont des tips, et on
     * maintient dans `tips[]` les `max_tips` plus récents (insertion
     * triée). Total de tips réels conservé dans `total` pour
     * exposition via total_tips_found.
     *
     * Coût total : O(n² + n × max_tips). Pour n=250 et max_tips=2,
     * c'est ~62 750 comparaisons, négligeable (< 1 ms sur ESP32).
     */
    for (uint32_t i = 0; i < dag->count; i++) {
        bool is_tip = true;
        const hash_t *candidate_id = &dag->transactions[i].id;

        /* Chercher si une autre TX référence cette TX comme parent */
        for (uint32_t j = 0; j < dag->count; j++) {
            if (i == j) continue;

            for (uint8_t p = 0; p < dag->transactions[j].parent_count; p++) {
                if (hash_equal(candidate_id, &dag->transactions[j].parents[p])) {
                    is_tip = false;
                    break;
                }
            }
            if (!is_tip) break;
        }

        if (!is_tip) continue;

        total++;

        const transaction_t *candidate = &dag->transactions[i];

        if (*tip_count < max_tips) {
            /*
             * Le buffer de sortie n'est pas plein : insérer le candidat
             * à la bonne position (tri décroissant par timestamp).
             */
            uint32_t pos = *tip_count;
            while (pos > 0 &&
                   tips[pos - 1]->timestamp < candidate->timestamp) {
                tips[pos] = tips[pos - 1];
                pos--;
            }
            tips[pos] = candidate;
            (*tip_count)++;
        } else if (max_tips > 0 &&
                   tips[max_tips - 1]->timestamp < candidate->timestamp) {
            /*
             * Le buffer est plein, mais le candidat est plus récent que
             * le moins récent du buffer : on évince le dernier et on
             * insère le candidat à la bonne position.
             */
            uint32_t pos = max_tips - 1;
            while (pos > 0 &&
                   tips[pos - 1]->timestamp < candidate->timestamp) {
                tips[pos] = tips[pos - 1];
                pos--;
            }
            tips[pos] = candidate;
        }
        /* Sinon : candidat plus ancien que tous les tips déjà retenus, on
         * l'ignore. Il est néanmoins compté dans `total`. */
    }

    if (total_tips_found != NULL) {
        *total_tips_found = total;
    }

    xSemaphoreGiveRecursive(dag->mutex);
    return ESP_OK;
}

esp_err_t dag_get_tips(const dag_t *dag, const transaction_t **tips,
                       uint32_t max_tips, uint32_t *tip_count)
{
    /* Délègue à _ext en ignorant le total des tips trouvés. */
    return dag_get_tips_ext(dag, tips, max_tips, tip_count, NULL);
}

uint32_t dag_count(const dag_t *dag)
{
    if (dag == NULL) return 0;
    xSemaphoreTakeRecursive(dag->mutex, portMAX_DELAY);
    uint32_t count = dag->count;
    xSemaphoreGiveRecursive(dag->mutex);
    return count;
}

bool dag_contains(const dag_t *dag, const hash_t *id)
{
    /* dag_get_by_id prend déjà le mutex (récursif) */
    return dag_get_by_id(dag, id) != NULL;
}

esp_err_t dag_set_status(dag_t *dag, const hash_t *id, tx_status_t status)
{
    if (dag == NULL || id == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTakeRecursive(dag->mutex, portMAX_DELAY);

    /* Recherche linéaire — même logique que dag_get_by_id mais mutable */
    for (uint32_t i = 0; i < dag->count; i++) {
        if (hash_equal(&dag->transactions[i].id, id)) {
            tx_status_t current = dag->transactions[i].status;

            /*
             * [F-DG-017] Valider la transition. Le cycle de vie attendu
             * d'une TX est :
             *
             *   (creation) → LOCKED ── set_status ──> CONFIRMED  (terminal)
             *                  └─── set_status ──> CANCELLED (terminal)
             *   (creation) → CONFIRMED  (TX recue deja confirmee)
             *
             * Transitions interdites :
             *   - CONFIRMED → quoi que ce soit (etat terminal)
             *   - CANCELLED → quoi que ce soit (etat terminal)
             *   - LOCKED    → LOCKED (no-op suspect)
             *
             * Idempotence : on accepte same-status uniquement si le
             * statut courant est deja CONFIRMED ou CANCELLED (les
             * appelants peuvent rejouer une confirmation sans bug).
             * Pour LOCKED→LOCKED, on renvoie ESP_OK silencieux pour
             * eviter de casser les flux qui posent le statut sur une
             * TX qu'ils viennent de creer.
             */
            if (current == status) {
                xSemaphoreGiveRecursive(dag->mutex);
                return ESP_OK; /* idempotent */
            }

            bool allowed =
                (current == TX_STATUS_LOCKED &&
                 (status == TX_STATUS_CONFIRMED ||
                  status == TX_STATUS_CANCELLED));

            if (!allowed) {
                xSemaphoreGiveRecursive(dag->mutex);
                return ESP_ERR_INVALID_STATE;
            }

            dag->transactions[i].status = status;
            xSemaphoreGiveRecursive(dag->mutex);
            return ESP_OK;
        }
    }

    xSemaphoreGiveRecursive(dag->mutex);
    return ESP_ERR_NOT_FOUND;
}
