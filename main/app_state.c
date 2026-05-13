/**
 * @file app_state.c
 * @brief Definition (storage reelle) de l'etat global declare dans app_state.h.
 *
 * Tous les modules du firmware accedent a cet etat via le header partage.
 * Aucune logique ici — c'est uniquement de la storage. Les fonctions qui
 * manipulent cet etat vivent dans handlers/, ops/, persistence/, etc.
 */

#include "app_state.h"

/* ================================================================
 * Donnees metier protegees par s_state_mutex
 * ================================================================ */

dag_t              s_dag;
wallet_t           s_wallet;
lock_table_t       s_lock_table;
time_manager_t     s_time_manager;
currency_config_t  s_currency;
keypair_t          s_keypair;
checkpoint_t       s_checkpoint;

/* ================================================================
 * HAL
 * ================================================================ */

hal_storage_t      s_storage;
hal_display_t      s_display;
#ifdef MP_HAS_ESPNOW
espnow_hal_t       s_espnow_hal;
#endif
#ifdef MP_HAS_LORA
hal_lora_t         s_lora_hal;
#endif

/* ================================================================
 * Communication inter-taches
 * ================================================================ */

QueueHandle_t      s_evt_queue;
#ifdef MP_HAS_ESPNOW
QueueHandle_t      s_cmd_queue;
#endif
SemaphoreHandle_t  s_state_mutex;

/* ================================================================
 * UI
 * ================================================================ */

ui_ctx_t           s_ui_ctx;
QueueHandle_t      s_ui_cmd_queue;
volatile ui_pay_feedback_t s_pay_feedback = UI_PAY_FEEDBACK_NONE;

/* ================================================================
 * Peers et bookkeeping
 * ================================================================ */

comm_peer_info_t   s_peers[MAX_PEERS];
uint32_t           s_peer_count = 0;
uint64_t           s_last_expire_check = 0;
uint32_t           s_next_seq = 0;

/* ================================================================
 * Broadcasts et cache anti-boucle
 * ================================================================ */

comm_msg_broadcast_t s_pending_broadcast;
bool                 s_broadcast_pending = false;

signature_t s_seen_bcast[MAX_SEEN_BROADCASTS];
uint32_t    s_seen_bcast_count = 0;

#ifdef MP_HAS_LORA
uint8_t  s_relay_bcast_buf[COMM_MSG_LORA_MAX];
size_t   s_relay_bcast_len = 0;
bool     s_relay_bcast_pending = false;
#endif

/* ================================================================
 * Alias du device
 * ================================================================ */

char    s_device_alias[COMM_MSG_ALIAS_MAX + 1] = "Device";
uint8_t s_device_alias_len = 6;

/* ================================================================
 * Ping/pong LoRa
 * ================================================================ */

ping_result_t s_ping_results[MAX_PING_RESULTS];
uint32_t      s_ping_result_count = 0;
uint16_t      s_current_ping_id   = 0;
bool          s_ping_active       = false;

seen_ping_entry_t s_seen_pings[MAX_SEEN_PINGS];
uint32_t          s_seen_ping_count = 0;

#ifdef MP_HAS_LORA
uint8_t  s_relay_ping_buf[COMM_MSG_PING_SIZE];
size_t   s_relay_ping_len = 0;
bool     s_relay_ping_pending = false;

uint8_t    s_pong_buf[COMM_MSG_LORA_MAX];
size_t     s_pong_len = 0;
bool       s_pong_pending = false;
uint32_t   s_pong_delay_ms = 0;
TickType_t s_pong_start_tick = 0;
#endif

/* ================================================================
 * Auto-forward beneficiaire
 * ================================================================ */

public_key_t s_beneficiary_key;          /* Initialise a zero par defaut */
uint16_t     s_forward_interval_min = 0; /* 0 = inactif */
uint64_t     s_last_forward_ms = 0;
