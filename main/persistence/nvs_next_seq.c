/**
 * @file persistence/nvs_next_seq.c
 * @brief Implementation nonce monotone (voir header).
 */

#include "nvs_next_seq.h"

#include <inttypes.h>

#include "esp_log.h"

#include "app_state.h"
#include "transaction/tx_types.h"

static const char *TAG = "nvs_seq";

/*
 * [F-DG-011] Drapeau levé si la recovery au boot a dû fall-back à 0
 * sans aucune source de référence (NVS+DAG+checkpoint tous absents).
 * Exposé via seq_recovery_failed() pour l'UI.
 */
static bool s_seq_recovery_failed = false;

/* Helper interne : scan du DAG pour le max(seq) du propriétaire.
 * Retourne UINT32_MAX si aucune TX `from == self` n'existe. */
static uint32_t scan_dag_max_seq_own(void)
{
    uint32_t max_seq = 0;
    bool found_any = false;
    for (uint32_t i = 0; i < s_dag.count; i++) {
        const transaction_t *tx = &s_dag.transactions[i];
        if (public_key_equal(&tx->from, &s_keypair.public_key)) {
            if (!found_any || tx->seq > max_seq) {
                max_seq = tx->seq;
                found_any = true;
            }
        }
    }
    return found_any ? max_seq : UINT32_MAX;
}

uint32_t next_seq(void)
{
    uint32_t seq = s_next_seq;
    s_next_seq++;
    hal_err_t herr = s_storage.u32_write(NVS_NAMESPACE, NVS_KEY_NEXT_SEQ,
                                         s_next_seq, s_storage.ctx);
    if (herr != HAL_OK) {
        /*
         * Echec persistance : on continue quand meme. La persistance
         * protege contre les reboots, pas contre une corruption flash.
         * [F-DG-011] Si NVS_KEY_NEXT_SEQ est perdu, la recovery au boot
         * suivant retombera sur le DAG ou NVS_KEY_OWN_MAX_SEQ.
         */
        ESP_LOGW(TAG, "next_seq: echec persistance NVS (%d)", herr);
    }
    return seq;
}

void load_next_seq_or_recompute(void)
{
    s_seq_recovery_failed = false;

    /* (1) Source primaire : NVS_KEY_NEXT_SEQ. */
    uint32_t persisted = 0;
    hal_err_t herr = s_storage.u32_read(NVS_NAMESPACE, NVS_KEY_NEXT_SEQ,
                                        &persisted, s_storage.ctx);
    if (herr == HAL_OK) {
        s_next_seq = persisted;
        ESP_LOGI(TAG, "next_seq charge depuis NVS: %"PRIu32, s_next_seq);
        return;
    }

    /*
     * [F-DG-011] (2) NVS perdu : reconstituer depuis le DAG si non-vide.
     */
    uint32_t dag_max = scan_dag_max_seq_own();
    if (dag_max != UINT32_MAX) {
        s_next_seq = dag_max + 1;
        ESP_LOGI(TAG, "next_seq recompute depuis DAG: %"PRIu32, s_next_seq);
        return;
    }

    /*
     * [F-DG-011] (3) DAG vide ou sans TX du device : consulter le
     * filet de sauvegarde NVS_KEY_OWN_MAX_SEQ (mis à jour à chaque
     * checkpoint via nvs_persist_own_max_seq). Cette clé survit aux
     * prunes complets : même si le DAG a été entièrement consolidé
     * dans le checkpoint puis purgé, le max_seq est conservé.
     */
    uint32_t checkpoint_max = 0;
    herr = s_storage.u32_read(NVS_NAMESPACE, NVS_KEY_OWN_MAX_SEQ,
                              &checkpoint_max, s_storage.ctx);
    if (herr == HAL_OK) {
        s_next_seq = checkpoint_max + 1;
        ESP_LOGI(TAG, "next_seq recompute depuis NVS_KEY_OWN_MAX_SEQ "
                      "(checkpoint backup): %"PRIu32, s_next_seq);
        return;
    }

    /*
     * [F-DG-011] (4) Toutes les sources sont vides ou corrompues.
     * On part à 0 (premier boot légitime ou device complètement
     * réinitialisé) mais on lève le drapeau pour signaler à l'UI
     * que le device pourrait être banni par ses peers s'il a déjà
     * émis des TX dans le passé.
     */
    s_next_seq = 0;
    s_seq_recovery_failed = true;
    ESP_LOGW(TAG, "next_seq: aucune source de recovery (NVS+DAG+checkpoint "
                  "tous vides). Demarrage a 0 — RISQUE DE BAN si le device "
                  "a deja emis des TX (cf. F-DG-011)");
}

void nvs_persist_own_max_seq(void)
{
    /*
     * [F-DG-011] Calculer max(NVS existant, scan DAG, s_next_seq - 1)
     * pour ne JAMAIS écrire une valeur inférieure à ce qui est déjà
     * persisté (la monotonie est cruciale pour la propriété anti-rejeu).
     */
    uint32_t dag_max = scan_dag_max_seq_own();
    uint32_t existing = 0;
    hal_err_t herr_read = s_storage.u32_read(NVS_NAMESPACE,
                                              NVS_KEY_OWN_MAX_SEQ,
                                              &existing, s_storage.ctx);
    if (herr_read != HAL_OK) {
        existing = 0;
    }

    uint32_t new_value = existing;
    if (dag_max != UINT32_MAX && dag_max > new_value) {
        new_value = dag_max;
    }
    /* Inclure s_next_seq - 1 si présent : c'est le dernier seq émis
     * (s_next_seq pointe vers le prochain à utiliser). */
    if (s_next_seq > 0 && (s_next_seq - 1) > new_value) {
        new_value = s_next_seq - 1;
    }

    if (new_value == existing) {
        /* Pas d'évolution : on évite une écriture inutile (économie
         * d'usure flash sur les checkpoints fréquents). */
        return;
    }

    hal_err_t herr = s_storage.u32_write(NVS_NAMESPACE,
                                         NVS_KEY_OWN_MAX_SEQ,
                                         new_value, s_storage.ctx);
    if (herr != HAL_OK) {
        ESP_LOGW(TAG, "nvs_persist_own_max_seq: echec ecriture (%d)", herr);
        return;
    }
    ESP_LOGI(TAG, "Persisted own_max_seq=%"PRIu32" (checkpoint backup)",
             new_value);
}

bool seq_recovery_failed(void)
{
    return s_seq_recovery_failed;
}
