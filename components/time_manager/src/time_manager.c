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

/*
 * [F-TM-003] Protection contre les races multi-tâches FreeRTOS.
 *
 * time_manager_t est partagé entre au moins deux contextes sur ESP32 :
 *  - tâche LoRa (lora_sync_task) qui appelle time_manager_on_master_sync
 *    lors de la réception d'un COMM_MSG_LORA_TIME_SYNC ;
 *  - tâche core / transaction qui appelle time_manager_get_tx_timestamp
 *    et time_manager_on_tx_received quand une TX est créée/reçue.
 *
 * Les mises à jour de lamport_counter (uint64_t, donc deux mots de
 * 32 bits sur Xtensa LX7), master_offset_ms et master_valid sont
 * non-atomiques. Un read-modify-write interrompu peut produire une
 * valeur corrompue — recul du Lamport, offset partiellement écrit,
 * `master_valid == true` lu avant que `master_offset_ms` ait été
 * mis à jour, etc. → violation des invariants fondamentaux du DAG.
 *
 * Stratégie : spinlock portMUX_TYPE module-level. Le header public
 * documente que time_manager_t est mono-instance (alloué statique
 * dans main.c), donc un mutex de module sérialise correctement tous
 * les accès. Cohérent avec le pattern utilisé dans lora_sync.c
 * (s_frag_mux). Pas de modification de l'ABI publique de
 * time_manager_t — le header reste exempt de dépendance FreeRTOS.
 *
 * portMUX_TYPE est un spinlock léger, ISR-safe, qui désactive les
 * interruptions sur le cœur courant. Les sections critiques sont
 * extrêmement courtes (quelques lectures-écritures + comparaisons)
 * → impact négligeable sur la latence d'IRQ.
 */
#include "freertos/FreeRTOS.h"

static portMUX_TYPE s_tm_mux = portMUX_INITIALIZER_UNLOCKED;

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

    /*
     * [F-TM-003] Toute la séquence read-modify-write sur lamport_counter,
     * master_offset_ms et la lecture corrélée de master_valid /
     * last_master_update doit être atomique. get_monotonic() est
     * appelée à l'intérieur de la section critique parce qu'elle est
     * pure et rapide (esp_timer_get_time → lecture timer hardware).
     */
    taskENTER_CRITICAL(&s_tm_mux);

    uint64_t result;

    if (tm->mode == TIME_MODE_LAMPORT) {
        /* Mode Lamport : incrémenter et retourner */
        tm->lamport_counter++;
        result = tm->lamport_counter;
        taskEXIT_CRITICAL(&s_tm_mux);
        return result;
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
        result = tm->lamport_counter;
        taskEXIT_CRITICAL(&s_tm_mux);
        return result;
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
     *
     * [F-TM-005] Note importante : dès le premier sync MASTER valide,
     * lamport_counter prend une valeur de l'ordre du wall-clock
     * (millisecondes depuis l'epoch côté maître, typiquement 10⁹+
     * en production avec un RTC). Il ne représente donc plus un
     * compteur séquentiel "1, 2, 3…" comme en mode LAMPORT pur,
     * mais une borne basse de monotonie sur l'horloge globale.
     * Documenté aussi dans le header public.
     */
    tm->lamport_counter++;
    if (wall > tm->lamport_counter) {
        tm->lamport_counter = wall;
    }
    result = tm->lamport_counter;
    taskEXIT_CRITICAL(&s_tm_mux);
    return result;
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
     *
     * [F-TM-003] Lecture-modification du compteur sous spinlock pour
     * garantir l'atomicité de la séquence "compare puis assign puis
     * increment". Sans cela, deux on_tx_received concurrents pouvaient
     * perdre un incrément ou écraser une valeur déjà rafraîchie.
     */
    taskENTER_CRITICAL(&s_tm_mux);
    if (remote_ts > tm->lamport_counter) {
        tm->lamport_counter = remote_ts;
    }
    tm->lamport_counter++;
    taskEXIT_CRITICAL(&s_tm_mux);
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

    /* Rejet immédiat en mode Lamport — tm->mode est immuable après init. */
    if (tm->mode != TIME_MODE_MASTER) return -1;

    uint64_t local_now = tm->get_monotonic();

    /* Calculer le nouvel offset proposé */
    int64_t new_offset = (int64_t)master_timestamp - (int64_t)local_now;

    /*
     * [F-TM-003] Section critique : toute la séquence read-modify-write
     * sur master_valid, master_offset_ms, current_master_key,
     * last_master_update et lamport_counter doit être atomique vis-à-vis
     * des autres tâches qui lisent ces champs (notamment get_tx_timestamp
     * et get_lamport dans la tâche core). Sans cela, on pouvait observer
     * master_valid == true avec master_offset_ms encore non mis à jour,
     * ou recul du Lamport quand un on_tx_received concurrent rafraîchit
     * le compteur entre nos lectures et écritures.
     */
    taskENTER_CRITICAL(&s_tm_mux);

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
            taskEXIT_CRITICAL(&s_tm_mux);
            return -1;
        }

        /*
         * [F-TM-002] Pas de bascule de maître tant que le maître courant
         * est valide.
         *
         * L'ancienne logique "smallest delta wins" comparait |master_timestamp
         * - current_wall| avec |master_timestamp - (local_now + master_offset_ms)|,
         * deux expressions mathématiquement identiques — la condition
         * `delta >= current_master_delta` était donc TOUJOURS vraie et
         * tout maître alternatif était systématiquement rejeté. Le test
         * `master_multi_smallest_delta_wins` passait uniquement parce
         * qu'il vérifiait le cas où B était pire ; le cas où B est
         * meilleur n'était jamais couvert.
         *
         * Plutôt que d'introduire un critère ad hoc ("plus petit offset
         * absolu", "plus récent", etc.) dont la rigueur conceptuelle
         * serait discutable, on explicite le comportement de fait :
         * un maître adopté reste maître tant qu'il est entendu dans
         * la fenêtre TIME_MASTER_FALLBACK_MS. Une bascule légitime
         * passe par le fallback Lamport (timeout d'inactivité du maître
         * courant), puis ré-adoption d'un nouveau maître au "premier
         * sync entendu" via la branche else ci-dessous.
         *
         * On met à jour le Lamport même en cas de rejet — un maître
         * alternatif peut nous transmettre une horloge logique
         * supérieure que l'on doit refléter pour préserver la
         * monotonie globale.
         */
        if (!public_key_equal(&tm->current_master_key, master_key)) {
            if (master_lamport > tm->lamport_counter) {
                tm->lamport_counter = master_lamport;
            }
            tm->lamport_counter++;
            taskEXIT_CRITICAL(&s_tm_mux);
            return -1;
        }
    } else {
        /*
         * Premier maître entendu — pas de vérification de delta.
         * On fait confiance au premier signal reçu.
         *
         * [F-TM-001] Faille connue : un premier sync malveillant
         * (timestamp arbitrairement éloigné) sera adopté sans contrôle.
         * Corriger ce point implique un mécanisme plus subtil
         * (quorum de N syncs cohérents, persistance NVS de l'offset,
         * croisement avec Lamport, etc.) car la sémantique exacte de
         * master_timestamp varie selon l'émetteur (uptime du maître
         * vs wall-clock RTC vs Lamport global) et un seuil naïf
         * casserait les nœuds qui rejoignent un réseau déjà ancien.
         * À traiter dans un Lot ultérieur dédié à la robustesse du
         * consensus temporel.
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

    taskEXIT_CRITICAL(&s_tm_mux);
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

    /*
     * [F-TM-003] Lecture uint64_t non atomique sur Xtensa LX7 (deux
     * mots 32 bits). Une lecture concurrente d'un on_tx_received en
     * cours de modification renverrait un Lamport panaché (poids fort
     * d'avant l'incrément, poids faible d'après). Spinlock pour lire
     * la valeur en une fois.
     */
    taskENTER_CRITICAL(&s_tm_mux);
    uint64_t value = tm->lamport_counter;
    taskEXIT_CRITICAL(&s_tm_mux);
    return value;
}

bool time_manager_has_valid_master(const time_manager_t *tm)
{
    if (!tm || tm->mode != TIME_MODE_MASTER) return false;

    /*
     * [F-TM-003] Lecture corrélée de master_valid et last_master_update.
     * Sans lock, on pouvait lire master_valid == true (vu d'une ancienne
     * adoption) avec un last_master_update plus récent que la réalité
     * (vu d'un on_master_sync concurrent en cours), donnant un résultat
     * incohérent.
     */
    taskENTER_CRITICAL(&s_tm_mux);
    bool     valid_snapshot = tm->master_valid;
    uint64_t last_update    = tm->last_master_update;
    taskEXIT_CRITICAL(&s_tm_mux);

    if (!valid_snapshot) return false;

    uint64_t local_now = tm->get_monotonic();
    if ((local_now - last_update) > TIME_MASTER_FALLBACK_MS) {
        return false;
    }

    return true;
}
