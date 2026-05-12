/**
 * @file time_manager.h
 * @brief Abstraction de la gestion du temps pour le ledger décentralisé.
 *
 * Deux modes configurables au démarrage :
 *
 * Mode LAMPORT (défaut) :
 *   - Horloge logique Lamport pour l'ordonnancement du DAG
 *   - Compteur monotone croissant, incrémenté à chaque TX créée ou reçue
 *   - Aucune dépendance à un RTC ou un maître externe
 *
 * Mode MASTER :
 *   - Les devices maîtres diffusent leur RTC via LoRa (LORA_TIME_SYNC)
 *   - Les devices normaux adoptent le temps du maître (offset calculé)
 *   - Garde-fous : rejet delta > 1h, multi-maître → plus petit delta
 *   - Fallback automatique vers Lamport si aucun maître depuis 10 min
 *   - Le compteur Lamport reste maintenu en parallèle (jamais décroissant)
 *
 * Ce module fournit deux types de temps :
 *   1. Timestamp d'ordonnancement (Lamport ou wall-clock corrigé) → TX
 *   2. Temps monotonique local → timeouts (verrous 30s, fragments 10s)
 *
 * Le core/ ne dépend jamais de ce module au niveau CMake.
 * L'injection se fait via function pointers dans main.c.
 */

#ifndef TIME_MANAGER_H
#define TIME_MANAGER_H

#include "crypto/crypto_types.h"
#include <stdint.h>
#include <stdbool.h>

/* ================================================================
 * Types et constantes
 * ================================================================ */

/** Mode de gestion du temps */
typedef enum {
    TIME_MODE_LAMPORT = 0, /* Horloge logique Lamport (défaut) */
    TIME_MODE_MASTER  = 1, /* Temps synchronisé depuis un maître LoRa */
} time_mode_t;

/** Seuil de rejet du delta maître (1 heure en ms) */
#define TIME_MASTER_MAX_DELTA_MS   3600000ULL

/** Durée sans signal maître avant fallback vers Lamport (10 min en ms) */
#define TIME_MASTER_FALLBACK_MS    600000ULL

/**
 * Fonction fournissant le temps monotonique local en millisecondes.
 *
 * En production : wrapper autour de esp_timer_get_time() / 1000
 * En test : mock avec valeur contrôlable
 */
typedef uint64_t (*time_monotonic_fn)(void);

/* ================================================================
 * Configuration et état
 * ================================================================ */

/** Configuration du gestionnaire de temps (passée à init) */
typedef struct {
    time_mode_t       mode;           /* Mode de fonctionnement */
    time_monotonic_fn get_monotonic;  /* Source de temps monotonique (obligatoire) */
} time_manager_config_t;

/**
 * État du gestionnaire de temps.
 *
 * Alloué statiquement par l'appelant (typiquement dans main.c).
 * Toutes les opérations passent par les fonctions ci-dessous.
 */
typedef struct {
    time_mode_t       mode;            /* Mode actif */
    time_monotonic_fn get_monotonic;   /* Source monotonique injectée */

    /* Compteur Lamport — maintenu dans les deux modes */
    uint64_t          lamport_counter;

    /* Synchronisation maître (Mode MASTER uniquement) */
    int64_t           master_offset_ms;     /* offset = master_time - local_monotonic */
    uint64_t          last_master_update;   /* Timestamp monotonique du dernier sync */
    public_key_t      current_master_key;   /* Clé publique du maître adopté */
    bool              master_valid;         /* true si un maître a été entendu */
} time_manager_t;

/* ================================================================
 * API publique
 * ================================================================ */

/**
 * Initialiser le gestionnaire de temps.
 *
 * @param tm     [out] État à initialiser
 * @param config Configuration (mode + source monotonique)
 * @return 0 en cas de succès, -1 si paramètre invalide
 */
int time_manager_init(time_manager_t *tm, const time_manager_config_t *config);

/**
 * Obtenir le timestamp d'ordonnancement pour une nouvelle TX.
 *
 * Mode LAMPORT : incrémente le compteur Lamport et retourne la valeur.
 * Mode MASTER  : retourne le wall-clock corrigé (ou fallback Lamport).
 *                Le résultat est toujours >= lamport_counter (monotone).
 *
 * @param tm [in,out] Gestionnaire de temps
 * @return Timestamp d'ordonnancement (jamais 0, jamais décroissant)
 */
uint64_t time_manager_get_tx_timestamp(time_manager_t *tm);

/**
 * Mettre à jour l'horloge après réception d'une TX distante.
 *
 * Mode LAMPORT : local = max(local, remote_ts) + 1
 * Mode MASTER  : met à jour le Lamport interne uniquement
 *
 * Doit être appelé pour chaque TX reçue (ESP-NOW ou LoRa)
 * AVANT de traiter la TX (insertion DAG, etc.).
 *
 * @param tm        [in,out] Gestionnaire de temps
 * @param remote_ts Timestamp de la TX reçue
 */
void time_manager_on_tx_received(time_manager_t *tm, uint64_t remote_ts);

/**
 * Traiter un message de synchronisation temporelle du maître.
 *
 * Mode MASTER uniquement. Calcule l'offset et l'adopte si :
 * - Le delta est < TIME_MASTER_MAX_DELTA_MS (1h)
 * - En cas de multi-maître : le delta est le plus petit observé
 *
 * En mode LAMPORT, cette fonction retourne -1 sans effet.
 *
 * @param tm               [in,out] Gestionnaire de temps
 * @param master_key       Clé publique du maître émetteur
 * @param master_timestamp Timestamp wall-clock du maître (ms)
 * @param master_lamport   Valeur Lamport du maître
 * @return 0 si adopté, -1 si rejeté
 */
int time_manager_on_master_sync(time_manager_t *tm,
                                const public_key_t *master_key,
                                uint64_t master_timestamp,
                                uint64_t master_lamport);

/**
 * Obtenir le temps monotonique courant (pour les timeouts).
 *
 * Délègue directement à la fonction monotonique injectée.
 * Identique dans les deux modes. Utilisé pour :
 * - Timeout des verrous (30s)
 * - Timeout de réassemblage des fragments LoRa (10s)
 *
 * @param tm Gestionnaire de temps
 * @return Temps monotonique en millisecondes
 */
uint64_t time_manager_get_monotonic(const time_manager_t *tm);

/**
 * Obtenir la valeur Lamport courante sans incrémenter.
 *
 * Utilisé par les devices maîtres pour inclure le Lamport
 * dans les paquets LORA_TIME_SYNC.
 *
 * @param tm Gestionnaire de temps
 * @return Valeur Lamport courante
 */
uint64_t time_manager_get_lamport(const time_manager_t *tm);

/**
 * Vérifier si le mode maître a un maître valide et récent.
 *
 * Retourne false si :
 * - Le mode est LAMPORT
 * - Aucun maître n'a été entendu
 * - Le dernier signal date de plus de TIME_MASTER_FALLBACK_MS
 *
 * @param tm Gestionnaire de temps
 * @return true si un maître valide est actif
 */
bool time_manager_has_valid_master(const time_manager_t *tm);

#endif /* TIME_MANAGER_H */
