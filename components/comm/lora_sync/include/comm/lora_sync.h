/**
 * @file lora_sync.h
 * @brief Module de synchronisation LoRa périodique.
 *
 * Toutes les 2 minutes, ce module broadcast les transactions CONFIRMED
 * récentes via LoRa pour que les devices voisins puissent mettre à
 * jour leur DAG.
 *
 * Côté réception :
 * - Les TX simples (LORA_TX) sont décodées et postées comme événements
 * - Les fragments (LORA_FRAG) sont réassemblés via lora_frag
 *
 * Architecture (refonte Lot C item 7) :
 * - lora_sync_task() est la boucle principale FreeRTOS
 * - Le module ne connait PLUS le DAG ni le mutex applicatif. Il appelle
 *   un callback `collect_confirmed_txs` que main.c implemente : ce
 *   callback gere lui-meme le verrouillage interne, copie les TX a
 *   diffuser dans un buffer, et rend la main rapidement. Le composant
 *   reste ainsi decouple de la representation interne du DAG et de
 *   la strategie de verrouillage applicative (inversion de dependance).
 * - Le module n'ECRIT JAMAIS dans le DAG — les TX recues sont postees
 *   dans evt_queue et traitees par core_task.
 */

#ifndef LORA_SYNC_H
#define LORA_SYNC_H

#include "hal/hal_lora.h"
#include "hal/hal_types.h"
#include "comm/comm_event.h"
#include "crypto/crypto_types.h"
#include "transaction/tx_types.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

/** Intervalle de sync par défaut en millisecondes (2 minutes) */
#define LORA_SYNC_DEFAULT_INTERVAL_MS 120000

/**
 * Fonction fournissant le temps en millisecondes.
 * Même signature que get_time_ms_fn dans wallet.h.
 */
typedef uint64_t (*lora_get_time_fn)(void);

/**
 * Callback fournissant les TX CONFIRMED a diffuser sur le prochain
 * cycle de synchronisation LoRa.
 *
 * L'implementation doit :
 *   1. Acquerir tout verrou applicatif necessaire (mutex DAG, etc.).
 *   2. Parcourir le DAG et copier dans out_buf les TX CONFIRMED dont
 *      le timestamp est strictement superieur a since_ts, jusqu'a
 *      max_count entrees.
 *   3. Renseigner *out_newest_ts avec le plus grand timestamp copie
 *      (ou since_ts si rien n'a ete copie).
 *   4. Liberer le verrou.
 *   5. Retourner le nombre de TX copiees.
 *
 * Important : la fonction doit etre rapide (sous verrou applicatif).
 * Pas d'I/O reseau ni d'allocation dynamique a l'interieur.
 *
 * @param since_ts       Ne copier que les TX avec timestamp > since_ts
 * @param out_buf        Buffer de sortie (capacite max_count entrees)
 * @param max_count      Capacite max de out_buf
 * @param out_newest_ts  [out] Plus grand timestamp copie (ou since_ts)
 * @param ctx            Pointeur de contexte fourni par main.c
 * @return Nombre de TX effectivement copiees dans out_buf
 */
typedef uint32_t (*lora_collect_confirmed_txs_fn)(
    uint64_t        since_ts,
    transaction_t  *out_buf,
    uint32_t        max_count,
    uint64_t       *out_newest_ts,
    void           *ctx);

/**
 * Configuration du module de synchronisation LoRa.
 *
 * Passée à lora_sync_task() comme pvParameters.
 */
typedef struct {
    hal_lora_t       *lora;             /* Vtable LoRa du HAL */
    QueueHandle_t     evt_queue;        /* Queue comm → core_task */

    /* [Lot C item 7] Source des TX a diffuser : callback fourni par
     * main.c. Ce decouplage remplace l'ancien duo (dag, dag_mutex)
     * qui faisait remonter un mutex applicatif jusque dans le
     * composant. */
    lora_collect_confirmed_txs_fn collect_confirmed_txs;
    void             *collect_ctx;      /* Passe tel quel au callback */

    uint32_t          sync_interval_ms; /* Intervalle de sync (défaut 120000) */

    /* Synchronisation temporelle maître (optionnel) */
    bool              is_master;        /* true si ce device est un maître temporel */
    lora_get_time_fn  get_time;         /* Source de temps monotonique (pour fragments + master sync) */
    const public_key_t *own_pubkey;     /* Clé publique de ce device (nécessaire si is_master) */
    const keypair_t  *own_keypair;      /* Paire de clés pour signer les messages (nécessaire si is_master) */
    uint64_t        (*get_lamport)(void); /* Getter Lamport du maître (nécessaire si is_master) */
} lora_sync_config_t;

/**
 * Point d'entrée de la tâche FreeRTOS de synchronisation LoRa.
 *
 * Boucle infinie :
 * 1. Dormir sync_interval_ms
 * 2. Appeler collect_confirmed_txs pour recuperer les TX a diffuser
 * 3. Envoyer chaque TX via LoRa (1 paquet par TX)
 * 4. Retourner en mode réception
 *
 * Le callback RX traite les paquets entrants entre les cycles de sync.
 *
 * @param param Pointeur vers lora_sync_config_t
 */
void lora_sync_task(void *param);

/* ================================================================
 * Fonctions internes exposées pour les tests
 * ================================================================ */

/**
 * Traiter un paquet LoRa reçu.
 *
 * Décode le type (LORA_TX ou LORA_FRAG) et :
 * - LORA_TX : désérialise et poste COMM_EVT_LORA_TX_RECEIVED
 * - LORA_FRAG : accumule dans le contexte de réassemblage
 *
 * @param config Configuration du module
 * @param data   Données brutes du paquet
 * @param len    Taille des données
 */
void lora_sync_handle_rx(const lora_sync_config_t *config,
                          const uint8_t *data, size_t len);

/**
 * Effectuer un cycle de synchronisation.
 *
 * Appelle le callback collect_confirmed_txs et envoie les TX retournees
 * via LoRa.
 *
 * @param config       Configuration du module
 * @param last_sync_ts [in/out] Timestamp de la dernière sync. Les TX
 *                     avec timestamp > last_sync_ts sont envoyées.
 *                     Mis à jour avec le timestamp le plus récent envoyé.
 */
void lora_sync_do_cycle(const lora_sync_config_t *config,
                         uint64_t *last_sync_ts);

#endif /* LORA_SYNC_H */
