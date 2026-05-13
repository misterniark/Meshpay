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
 * MP_HAS_LORA   : LoRa Wio-E5 sur la carte CYD (ESP32 uniquement).
 */
#if CONFIG_IDF_TARGET_ESP32 || CONFIG_IDF_TARGET_ESP32S3
#define MP_HAS_ESPNOW 1
#endif
#if CONFIG_IDF_TARGET_ESP32
#define MP_HAS_LORA 1
#endif

#ifdef MP_HAS_ESPNOW
#include "comm/espnow.h"
#endif
#ifdef MP_HAS_LORA
#include "comm/lora_sync.h"
#include "hal/hal_lora.h"
#endif

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

/** Intervalle de verification des expirations de locks (ms). */
#define LOCK_EXPIRE_INTERVAL_MS   5000

/** Capacite max de la table des peers. */
#define MAX_PEERS  10

#ifdef MP_HAS_LORA
/** Pins LoRa Wio-E5 (UART2) — ESP32 CYD uniquement. */
#define LORA_UART_NUM    2
#define LORA_TX_PIN     17
#define LORA_RX_PIN     16

/** Intervalle de sync LoRa (ms). */
#define LORA_SYNC_INTERVAL_MS  120000
#endif /* MP_HAS_LORA */

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
#ifdef MP_HAS_LORA
extern hal_lora_t         s_lora_hal;
#endif

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

#ifdef MP_HAS_LORA
/* Relay broadcast LoRa (rempli sous mutex, consomme apres release). */
extern uint8_t  s_relay_bcast_buf[COMM_MSG_LORA_MAX];
extern size_t   s_relay_bcast_len;
extern bool     s_relay_bcast_pending;
#endif

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

#ifdef MP_HAS_LORA
/* Relay PING LoRa. */
extern uint8_t  s_relay_ping_buf[COMM_MSG_PING_SIZE];
extern size_t   s_relay_ping_len;
extern bool     s_relay_ping_pending;

/* PONG differe (envoi hors mutex apres delai jitter). */
extern uint8_t    s_pong_buf[COMM_MSG_LORA_MAX];
extern size_t     s_pong_len;
extern bool       s_pong_pending;
extern uint32_t   s_pong_delay_ms;
extern TickType_t s_pong_start_tick;
#endif

/* Auto-forward beneficiaire. */
extern public_key_t s_beneficiary_key;
extern uint16_t     s_forward_interval_min;
extern uint64_t     s_last_forward_ms;

/*
 * Note : les callbacks de persistance checkpoint (s_checkpoint_save /
 * s_checkpoint_load) restent dans main.c au Lot 1 car ils referencent
 * des fonctions statiques (nvs_checkpoint_save/load) qui migreront vers
 * components/main/persistence/nvs_checkpoint.c au Lot 2.
 */

#endif /* MESHPAY_APP_STATE_H */
