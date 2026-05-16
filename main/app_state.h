/**
 * @file app_state.h
 * @brief Etat global partage du firmware Mesh Pay.
 *
 * Centralise toutes les variables d'etat (DAG, wallet, locks, time_manager,
 * currency, HAL, queues, mutex) qui etaient auparavant `static` dans main.c.
 *
 * Pourquoi extraire ?
 *   Au Lot D du refactor (2026-05-13), main.c avait grossi a 3521 lignes
 *   et regroupait l'etat, les handlers, les operations maitre, la persistance,
 *   le boot, etc. Pour permettre la decomposition en modules separes
 *   (handlers/, ops/, persistance/) sans transformer chaque module en
 *   labyrinthe de getters, on expose l'etat via des declarations `extern`
 *   ici. La storage reelle est dans app_state.c.
 *
 * Discipline d'usage :
 *   - Toute mutation de DAG, wallet, lock_table, checkpoint, time_manager
 *     doit etre encadree par xSemaphoreTake/Give(s_state_mutex).
 *   - Les modules qui ont besoin d'acceder a l'etat incluent simplement
 *     ce header. Aucune duplication.
 *   - Les constantes de configuration (tailles de queues, periodes, etc.)
 *     restent ici car referencees par plusieurs modules.
 */

#ifndef MESHPAY_APP_STATE_H
#define MESHPAY_APP_STATE_H

#include <stdbool.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

#include "crypto/crypto_types.h"
#include "transaction/tx_types.h"
#include "dag/dag.h"
#include "wallet/wallet.h"
#include "wallet/wallet_lock.h"
#include "wallet/wallet_checkpoint.h"
#include "comm/comm_event.h"
#include "currency/currency_config.h"
#include "time_manager/time_manager.h"
#include "hal/hal_storage.h"
#include "hal/hal_display.h"
#include "ui/ui_state.h"

/*
 * Capabilites compilees selon la cible materielle.
 * Defini ici pour pouvoir gater les includes specifiques (ESP-NOW, LoRa)
 * et les declarations de HAL associees.
 *
 * MP_HAS_ESPNOW : ESP-NOW sur tout device avec radio Wi-Fi (ESP32 + ESP32-S3).
 */
#if CONFIG_IDF_TARGET_ESP32 || CONFIG_IDF_TARGET_ESP32S3
#define MP_HAS_ESPNOW 1
#endif

#ifdef MP_HAS_ESPNOW
#include "comm/espnow.h"
#endif
/*
 * NB Lot D.3 : les types LoRa (hal_lora_t, lora_sync_config_t) sont
 * consommes uniquement par transport/transport_lora.c. On n'inclut plus
 * les headers ici pour eviter que app_state.h porte la transitive.
 */

/* ================================================================
 * Constantes globales (queues, stacks, priorites, NVS, etc.)
 * ================================================================ */

/** Profondeur des queues inter-taches. */
#define EVT_QUEUE_DEPTH      16
#define CMD_QUEUE_DEPTH       8
#define UI_CMD_QUEUE_DEPTH    8

/**
 * Tailles de stack des taches (en mots de 4 octets).
 *
 * Augmentees au Lot C item 8 suite a l'audit : espnow/lora font de la crypto
 * (Ed25519, SHA-256) + CBOR sur des buffers de plusieurs centaines d'octets.
 * core_task agrege tout le traitement metier + DAG.
 */
#define ESPNOW_TASK_STACK  6144
#define CORE_TASK_STACK   10240
#define LORA_TASK_STACK    6144
#define UI_TASK_STACK      8192

/** Periode de log des high-water-marks de stack (ms). */
#define STACK_MONITOR_PERIOD_MS  30000

/**
 * Stack de la tache de monitoring.
 * Augmentee de 2048 a 4096 mots au Lot E.6 (2026-05-12) suite a un stack
 * overflow detecte sur Waveshare ESP32-S3 toutes les 30 s.
 */
#define STACK_MONITOR_TASK_STACK 4096

/** Priorites FreeRTOS. */
#define ESPNOW_TASK_PRIO      7
#define CORE_TASK_PRIO        6
#define LORA_TASK_PRIO        5
#define UI_TASK_PRIO          4

/** Cles NVS pour la persistance. */
#define NVS_NAMESPACE         "payment"
#define NVS_KEY_PRIVKEY       "privkey"
#define NVS_KEY_PUBKEY        "pubkey"
#define NVS_KEY_CHECKPOINT    "checkpoint"
#define NVS_KEY_FIRST_BOOT    "first_boot"
#define NVS_KEY_ALIAS         "alias"
#define NVS_KEY_BENEFICIARY   "beneficiary"
#define NVS_KEY_FWD_INTERVAL  "fwd_intv"
/* [I3-fix] Nonce monotone de ce device (incremente a chaque TX emise). */
#define NVS_KEY_NEXT_SEQ      "next_seq"
/*
 * [F-DG-011] Sauvegarde du max(seq) du propriétaire au moment de
 * chaque checkpoint. Sert de filet de récupération si NVS_KEY_NEXT_SEQ
 * est corrompu/effacé ET que le DAG est vide (post-prune complet).
 * Sans cette clé, le device repartait à seq=0 et était banni par les
 * peers qui avaient encore l'historique en mémoire.
 */
#define NVS_KEY_OWN_MAX_SEQ   "own_max_seq"

/** Intervalle de verification des expirations de locks (ms). */
#define LOCK_EXPIRE_INTERVAL_MS   5000

/** Capacite max de la table des peers. */
/*
 * [F-MN-011] Augmenté de 10 à 32 le 2026-05-16 pour couvrir les
 * déploiements moyens (festivals jusqu'à ~32 participants en
 * simultané). Combiné avec une politique LRU dans peers.c, les
 * nouveaux peers expulsent le plus ancien plutôt que d'être
 * silencieusement rejetés.
 * Coût mémoire : 32 × ~70 octets ≈ 2.2 Ko de BSS.
 */
#define MAX_PEERS  32

/*
 * Pins LoRa Wio-E5 et intervalle de sync : deplaces dans transport_lora.c
 * (Lot D.3, prives a l'impl reelle).
 */

/** Cache anti-boucle pour les broadcasts (taille du buffer circulaire). */
#define MAX_SEEN_BROADCASTS 16

/** Capacite max des resultats de ping (PONGs collectes). */
#define MAX_PING_RESULTS 32

/** Cache anti-boucle pour les pings (taille du buffer circulaire). */
#define MAX_SEEN_PINGS 4

/* ================================================================
 * Types auxiliaires (utilises uniquement dans l'etat global)
 * ================================================================ */

/** Resultat de PONG collecte par le maitre apres un PING. */
typedef struct {
    public_key_t key;
    char         alias[COMM_MSG_ALIAS_MAX + 1];
    uint8_t      alias_len;
} ping_result_t;

/** Entree du cache anti-boucle pour les pings (par maitre + ping_id). */
typedef struct {
    public_key_t master_key;
    uint16_t     ping_id;
} seen_ping_entry_t;

/* ================================================================
 * Etat global (~120 KB) — defini dans app_state.c
 * ================================================================ */

/* Donnees metier protegees par s_state_mutex. */
extern dag_t              s_dag;             /* ~116 KB (500 TX x 232 octets) */
extern wallet_t           s_wallet;
extern lock_table_t       s_lock_table;
extern time_manager_t     s_time_manager;
extern currency_config_t  s_currency;
extern keypair_t          s_keypair;
extern checkpoint_t       s_checkpoint;

/* HAL. */
extern hal_storage_t      s_storage;
extern hal_display_t      s_display;
#ifdef MP_HAS_ESPNOW
extern espnow_hal_t       s_espnow_hal;
#endif
/* s_lora_hal est prive a transport/transport_lora.c (Lot D.3). */

/* Communication inter-taches. */
extern QueueHandle_t      s_evt_queue;
#ifdef MP_HAS_ESPNOW
extern QueueHandle_t      s_cmd_queue;
#endif
extern SemaphoreHandle_t  s_state_mutex;

/* UI. */
extern ui_ctx_t           s_ui_ctx;
extern QueueHandle_t      s_ui_cmd_queue;
extern volatile ui_pay_feedback_t s_pay_feedback;

/* Peers decouverts. */
extern comm_peer_info_t   s_peers[MAX_PEERS];
extern uint32_t           s_peer_count;

/* Verification periodique des locks. */
extern uint64_t           s_last_expire_check;

/* [I3-fix] Nonce monotone de ce device. */
extern uint32_t           s_next_seq;

/* Dernier broadcast maitre recu (pour l'UI). */
extern comm_msg_broadcast_t s_pending_broadcast;
extern bool                 s_broadcast_pending;

/* Cache anti-boucle des broadcasts (signature deja vue). */
extern signature_t s_seen_bcast[MAX_SEEN_BROADCASTS];
extern uint32_t    s_seen_bcast_count;

/* Buffers de relay/pong LoRa : prives a transport/transport_lora.c (Lot D.3). */

/* Alias du device (charge depuis NVS ou genere au premier boot). */
extern char    s_device_alias[COMM_MSG_ALIAS_MAX + 1];
extern uint8_t s_device_alias_len;

/* Etat du ping/pong LoRa (maitre uniquement). */
extern ping_result_t s_ping_results[MAX_PING_RESULTS];
extern uint32_t      s_ping_result_count;
extern uint16_t      s_current_ping_id;
extern bool          s_ping_active;

/* Cache anti-boucle des pings. */
extern seen_ping_entry_t s_seen_pings[MAX_SEEN_PINGS];
extern uint32_t          s_seen_ping_count;

/* s_relay_ping_*, s_pong_* : prives a transport/transport_lora.c (Lot D.3). */

/* Auto-forward beneficiaire. */
extern public_key_t s_beneficiary_key;
extern uint16_t     s_forward_interval_min;
extern uint64_t     s_last_forward_ms;

/*
 * Callbacks de persistance checkpoint (Lot 2).
 *
 * En production : initialises avec nvs_checkpoint_save / nvs_checkpoint_load
 * (composant main/persistence). En test, peuvent etre reassignes avant le
 * boot pour brancher un backend memoire/SD/SPIFFS.
 *
 * L'initialisation se fait dans app_main (cf. seance 3c).
 */
extern checkpoint_save_fn s_checkpoint_save;
extern checkpoint_load_fn s_checkpoint_load;

#endif /* MESHPAY_APP_STATE_H */
