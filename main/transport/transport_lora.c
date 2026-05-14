/**
 * @file transport/transport_lora.c
 * @brief Implementation reelle de la facade LoRa (cibles equipees d'un Wio-E5).
 *
 * Compile sur les cibles qui embarquent un module LoRa Wio-E5 : ESP32 CYD
 * et ESP32-S3 Waveshare (cf. CMakeLists.txt). Sur d'eventuelles autres
 * cibles, c'est `transport_lora_stub.c` qui est lie.
 *
 * Owne toute la couche LoRa : HAL physique, buffers relay/pong, callbacks
 * pour lora_sync_task. Le reste du firmware n'a plus aucun `#ifdef` autour
 * d'un appel LoRa.
 */

#include "transport_lora.h"

#include <inttypes.h>
#include <string.h>

#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "app_state.h"
#include "comm/comm_msg.h"
#include "comm/lora_sync.h"
#include "hal/hal_lora.h"
#include "time_glue.h"
#include "transaction/tx_types.h"

extern hal_err_t hal_lora_wio_e5_create(hal_lora_t *lora, int uart_num,
                                        int tx_pin, int rx_pin);

static const char *TAG = "tport_lora";

/* ----------------------------------------------------------------
 * Constantes specifiques LoRa (anciennement dans app_state.h)
 * ---------------------------------------------------------------- */

/* Pins LoRa Wio-E5 — selon la cible materielle. */
#if CONFIG_IDF_TARGET_ESP32
/* CYD (ESP32-2432S028) : UART2, broches libres du header. */
#define LORA_UART_NUM    2
#define LORA_TX_PIN     17
#define LORA_RX_PIN     16
#elif CONFIG_IDF_TARGET_ESP32S3
/* Waveshare ESP32-S3-Touch-LCD-1.47 : UART1 sur GPIO 43/44 (broches U0
 * du header d'extension, libres car la console serie passe par
 * l'USB-Serial-JTAG). Cablage : S3 GPIO43 -> Wio RX, S3 GPIO44 -> Wio TX. */
#define LORA_UART_NUM    1
#define LORA_TX_PIN     43
#define LORA_RX_PIN     44
#else
/* Garde-fou : ce fichier ne doit etre compile que sur une cible avec
 * Wio-E5 (cf. gate CMakeLists). Toute nouvelle cible doit definir son
 * propre pinout LoRa ici. */
#error "transport_lora.c compile sur une cible sans pinout Wio-E5 defini"
#endif

/** Intervalle de sync LoRa (ms). */
#define LORA_SYNC_INTERVAL_MS  120000

/* ----------------------------------------------------------------
 * Etat prive : HAL, buffers, config lora_sync_task
 * ---------------------------------------------------------------- */

static hal_lora_t s_lora_hal;

/* Buffers de relay/pong (precedemment dans app_state.{h,c}). */
static uint8_t  s_relay_bcast_buf[COMM_MSG_LORA_MAX];
static size_t   s_relay_bcast_len = 0;
static bool     s_relay_bcast_pending = false;

static uint8_t  s_relay_ping_buf[COMM_MSG_PING_SIZE];
static size_t   s_relay_ping_len = 0;
static bool     s_relay_ping_pending = false;

static uint8_t    s_pong_buf[COMM_MSG_LORA_MAX];
static size_t     s_pong_len = 0;
static bool       s_pong_pending = false;
static uint32_t   s_pong_delay_ms = 0;
static TickType_t s_pong_start_tick = 0;

/* Config persistante de lora_sync_task (passee par pointeur, doit survivre). */
static lora_sync_config_t s_lora_cfg;

/* ----------------------------------------------------------------
 * Callbacks pour lora_sync (etaient dans time_glue avant le Lot D.3)
 * ---------------------------------------------------------------- */

/**
 * Wrapper pour obtenir le compteur Lamport courant (broadcast TIME_SYNC).
 */
static uint64_t lora_get_lamport(void)
{
    return time_manager_get_lamport(&s_time_manager);
}

/**
 * Callback de collecte des TX a diffuser via LoRa.
 *
 * Prend s_state_mutex (timeout 1 s), copie les TX CONFIRMED dont
 * timestamp > since_ts dans out_buf. Si le mutex est indisponible,
 * cycle saute (logge en WARN), retourne 0.
 *
 * Implemente la signature `lora_collect_confirmed_txs_fn`.
 */
static uint32_t lora_collect_confirmed_txs(uint64_t       since_ts,
                                           transaction_t *out_buf,
                                           uint32_t       max_count,
                                           uint64_t      *out_newest_ts,
                                           void          *ctx)
{
    (void)ctx;
    if (!out_buf || !out_newest_ts || max_count == 0) {
        if (out_newest_ts) *out_newest_ts = since_ts;
        return 0;
    }

    uint64_t newest  = since_ts;
    uint32_t written = 0;

    /*
     * Timeout 1 s (parallelisme avec l'ancien code) : si core_task
     * tient le mutex trop longtemps, on saute ce cycle ; le prochain
     * reessaiera.
     */
    if (xSemaphoreTake(s_state_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        ESP_LOGW(TAG, "Sync LoRa : mutex indisponible, cycle saute");
        *out_newest_ts = newest;
        return 0;
    }

    for (uint32_t i = 0; i < s_dag.count && written < max_count; i++) {
        const transaction_t *tx = &s_dag.transactions[i];
        if (tx->status == TX_STATUS_CONFIRMED && tx->timestamp > since_ts) {
            memcpy(&out_buf[written], tx, sizeof(transaction_t));
            if (tx->timestamp > newest) newest = tx->timestamp;
            written++;
        }
    }

    xSemaphoreGive(s_state_mutex);

    *out_newest_ts = newest;
    return written;
}

/* ----------------------------------------------------------------
 * API publique
 * ---------------------------------------------------------------- */

bool transport_lora_available(void)
{
    return true;
}

hal_err_t transport_lora_init_and_start(void)
{
    /* 1. HAL physique. */
    hal_err_t err = hal_lora_wio_e5_create(&s_lora_hal, LORA_UART_NUM,
                                           LORA_TX_PIN, LORA_RX_PIN);
    if (err != HAL_OK) {
        ESP_LOGW(TAG, "HAL LoRa init echoue (%d) — fonctionnement sans LoRa", err);
        return err;
    }

    /*
     * 2. Tache lora_sync.
     *
     * [Lot C item 7] Le DAG et le mutex applicatif ne sont plus passes
     * directement au composant : on lui fournit un callback de collecte
     * qui gere lui-meme le verrouillage. Decouplage propre.
     */
    memset(&s_lora_cfg, 0, sizeof(s_lora_cfg));
    s_lora_cfg.lora                   = &s_lora_hal;
    s_lora_cfg.evt_queue              = s_evt_queue;
    s_lora_cfg.collect_confirmed_txs  = lora_collect_confirmed_txs;
    s_lora_cfg.collect_ctx            = NULL;
    s_lora_cfg.sync_interval_ms       = LORA_SYNC_INTERVAL_MS;
    s_lora_cfg.is_master              = false;
    s_lora_cfg.get_time               = get_time_ms_wrapper;
    s_lora_cfg.own_pubkey             = &s_keypair.public_key;
    s_lora_cfg.own_keypair            = &s_keypair;
    s_lora_cfg.get_lamport            = lora_get_lamport;

    xTaskCreate(lora_sync_task, "lora", LORA_TASK_STACK, &s_lora_cfg,
                LORA_TASK_PRIO, NULL);
    return HAL_OK;
}

bool transport_lora_send(const uint8_t *buf, size_t len, const char *what)
{
    hal_err_t herr = s_lora_hal.send(buf, len, s_lora_hal.ctx);
    if (herr != HAL_OK) {
        ESP_LOGW(TAG, "Echec envoi LoRa (%s): %d", what ? what : "?", herr);
        return false;
    }
    return true;
}

void transport_lora_queue_relay_broadcast(const uint8_t *buf, size_t len)
{
    if (len > sizeof(s_relay_bcast_buf)) return;
    memcpy(s_relay_bcast_buf, buf, len);
    s_relay_bcast_len = len;
    s_relay_bcast_pending = true;
}

void transport_lora_queue_relay_ping(const uint8_t *buf, size_t len)
{
    if (len > sizeof(s_relay_ping_buf)) return;
    memcpy(s_relay_ping_buf, buf, len);
    s_relay_ping_len = len;
    s_relay_ping_pending = true;
}

void transport_lora_queue_pong_delayed(const uint8_t *buf, size_t len,
                                       uint32_t delay_ms)
{
    if (len > sizeof(s_pong_buf)) return;
    memcpy(s_pong_buf, buf, len);
    s_pong_len = len;
    s_pong_delay_ms = delay_ms;
    s_pong_start_tick = xTaskGetTickCount();
    s_pong_pending = true;
}

void transport_lora_pump(void)
{
    /*
     * Relay broadcast LoRa. Delai aleatoire (200-1000 ms) anti-collision :
     * tous les devices recevant le broadcast en meme temps doivent eviter
     * de retransmettre simultanement sur le meme canal.
     */
    if (s_relay_bcast_pending) {
        s_relay_bcast_pending = false;
        uint32_t delay_ms = 200 + (esp_random() % 801);
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
        if (transport_lora_send(s_relay_bcast_buf, s_relay_bcast_len, "relay-bcast")) {
            ESP_LOGI(TAG, "Broadcast relaye (delai=%lums)", (unsigned long)delay_ms);
        }
    }

    if (s_relay_ping_pending) {
        s_relay_ping_pending = false;
        uint32_t delay_ms = 200 + (esp_random() % 801);
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
        if (transport_lora_send(s_relay_ping_buf, s_relay_ping_len, "relay-ping")) {
            ESP_LOGI(TAG, "PING relaye (delai=%lums)", (unsigned long)delay_ms);
        }
    }

    /*
     * PONG differe : prepare sous mutex avec un delai aleatoire 1-5 s,
     * envoye ici (hors mutex) lorsque le delai est ecoule. Evite de
     * bloquer core_task pendant plusieurs secondes sous lock.
     */
    if (s_pong_pending) {
        TickType_t elapsed = xTaskGetTickCount() - s_pong_start_tick;
        if (elapsed >= pdMS_TO_TICKS(s_pong_delay_ms)) {
            s_pong_pending = false;
            if (transport_lora_send(s_pong_buf, s_pong_len, "pong")) {
                ESP_LOGI(TAG, "PONG envoye (delai=%lums)",
                         (unsigned long)s_pong_delay_ms);
            }
        }
    }
}
