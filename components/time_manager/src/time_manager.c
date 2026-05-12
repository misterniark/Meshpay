/**
 * @file time_manager.c
 * @brief Implémentation de la gestion du temps (Lamport + Master).
 *
 * Deux modes :
 *
 * LAMPORT : compteur logique pur.
 *   - get_tx_timestamp() → counter++
 *   - on_tx_received(T) → counter = max(counter, T) + 1
 *
 * MASTER : wall-clock corrigé depuis un maître LoRa.
 *   - on_master_sync() → calcule offset = master_time - local_monotonic
 *   - get_tx_timestamp() → local_now + offset (mais jamais < Lamport)
 *   - Fallback Lamport si aucun maître depuis 10 min
 *
 * Le compteur Lamport est TOUJOURS maintenu (même en mode MASTER)
 * pour garantir la monotonie et servir de fallback.
 */

#include "time_manager/time_manager.h"
#include <string.h>

/* ================================================================
 * Initialisation
 * ================================================================ */

int time_manager_init(time_manager_t *tm, const time_manager_config_t *config)
{
    if (!tm || !config || !config->get_monotonic) {
        return -1;
    }

    memset(tm, 0, sizeof(time_manager_t));
    tm->mode          = config->mode;
    tm->get_monotonic = config->get_monotonic;

    /* Lamport commence à 0, sera incrémenté avant la première utilisation */
    tm->lamport_counter = 0;

    /* Master sync non valide au démarrage */
    tm->master_valid       = false;
    tm->master_offset_ms   = 0;
    tm->last_master_update = 0;

    return 0;
}

/* ================================================================
 * Timestamp d'ordonnancement
 * ================================================================ */

uint64_t time_manager_get_tx_timestamp(time_manager_t *tm)
{
    if (!tm) return 0;

    if (tm->mode == TIME_MODE_LAMPORT) {
        /* Mode Lamport : incrémenter et retourner */
        tm->lamport_counter++;
        return tm->lamport_counter;
    }

    /* Mode MASTER */

    /*
     * Vérifier si le maître est encore valide.
     * Si aucun maître entendu ou timeout dépassé → fallback Lamport.
     */
    uint64_t local_now = tm->get_monotonic();

    if (!tm->master_valid ||
        (local_now - tm->last_master_update) > TIME_MASTER_FALLBACK_MS) {
        /* Fallback vers Lamport */
        tm->lamport_counter++;
        return tm->lamport_counter;
    }

    /*
     * Calculer le wall-clock corrigé.
     * L'offset peut être négatif (le maître est "en retard" par rapport
     * à notre monotonique), donc on caste en int64 pour l'addition.
     */
    uint64_t wall = (uint64_t)((int64_t)local_now + tm->master_offset_ms);

    /*
     * Garantie de monotonie : le timestamp ne doit JAMAIS descendre
     * sous la valeur Lamport courante. Si le wall-clock corrigé est
     * inférieur au Lamport, on utilise le Lamport.
     */
    tm->lamport_counter++;
    if (wall > tm->lamport_counter) {
        tm->lamport_counter = wall;
    }
    return tm->lamport_counter;
}

/* ================================================================
 * Mise à jour sur réception de TX
 * ================================================================ */

void time_manager_on_tx_received(time_manager_t *tm, uint64_t remote_ts)
{
    if (!tm) return;

    /*
     * Règle de Lamport : max(local, remote) + 1
     * S'applique dans les deux modes — le Lamport est toujours maintenu.
     */
    if (remote_ts > tm->lamport_counter) {
        tm->lamport_counter = remote_ts;
    }
    tm->lamport_counter++;
}

/* ================================================================
 * Synchronisation maître
 * ================================================================ */

int time_manager_on_master_sync(time_manager_t *tm,
                                const public_key_t *master_key,
                                uint64_t master_timestamp,
                                uint64_t master_lamport)
{
    if (!tm || !master_key) return -1;

    /* Rejet immédiat en mode Lamport */
    if (tm->mode != TIME_MODE_MASTER) return -1;

    uint64_t local_now = tm->get_monotonic();

    /* Calculer le nouvel offset proposé */
    int64_t new_offset = (int64_t)master_timestamp - (int64_t)local_now;

    /*
     * Si on a déjà un maître valide, vérifier le delta.
     * Le delta est la différence entre le temps proposé et notre
     * temps corrigé actuel.
     */
    if (tm->master_valid) {
        /* Temps corrigé actuel */
        int64_t current_wall = (int64_t)local_now + tm->master_offset_ms;
        int64_t delta = (int64_t)master_timestamp - current_wall;
        if (delta < 0) delta = -delta;

        /* Rejet si delta > 1 heure */
        if ((uint64_t)delta > TIME_MASTER_MAX_DELTA_MS) {
            return -1;
        }

        /*
         * Multi-maître : garder celui avec le plus petit delta.
         * Si c'est un nouveau maître, on ne l'adopte que s'il a un
         * delta plus petit que le maître actuel.
         */
        if (!public_key_equal(&tm->current_master_key, master_key)) {
            /* Calculer le delta du maître actuel (devrait être ~0 si récent) */
            int64_t current_master_delta = (int64_t)master_timestamp -
                ((int64_t)local_now + tm->master_offset_ms);
            if (current_master_delta < 0) current_master_delta = -current_master_delta;

            /* Nouveau maître ne gagne que s'il a un plus petit delta */
            if (delta >= current_master_delta) {
                /* Garder le maître actuel, mais mettre à jour le Lamport */
                if (master_lamport > tm->lamport_counter) {
                    tm->lamport_counter = master_lamport;
                }
                tm->lamport_counter++;
                return -1;
            }
        }
    } else {
        /*
         * Premier maître entendu — pas de vérification de delta.
         * On fait confiance au premier signal reçu.
         */
    }

    /* Adopter ce maître */
    tm->master_offset_ms   = new_offset;
    tm->last_master_update = local_now;
    memcpy(&tm->current_master_key, master_key, sizeof(public_key_t));
    tm->master_valid = true;

    /* Mettre à jour le Lamport depuis le maître aussi */
    if (master_lamport > tm->lamport_counter) {
        tm->lamport_counter = master_lamport;
    }
    tm->lamport_counter++;

    return 0;
}

/* ================================================================
 * Fonctions utilitaires
 * ================================================================ */

uint64_t time_manager_get_monotonic(const time_manager_t *tm)
{
    if (!tm || !tm->get_monotonic) return 0;
    return tm->get_monotonic();
}

uint64_t time_manager_get_lamport(const time_manager_t *tm)
{
    if (!tm) return 0;
    return tm->lamport_counter;
}

bool time_manager_has_valid_master(const time_manager_t *tm)
{
    if (!tm || tm->mode != TIME_MODE_MASTER) return false;
    if (!tm->master_valid) return false;

    /* Vérifier le timeout de fallback */
    uint64_t local_now = tm->get_monotonic();
    if ((local_now - tm->last_master_update) > TIME_MASTER_FALLBACK_MS) {
        return false;
    }

    return true;
}
