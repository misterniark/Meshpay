/**
 * @file transport/transport_lora.c
 * @brief Implementation de la facade LoRa — toujours compilee, tous devices.
 *
 * Owne toute la couche LoRa : HAL physique, buffers relay/pong, callbacks
 * pour lora_sync_task. Le reste du firmware n'a aucun `#ifdef` autour
 * d'un appel LoRa.
 *
 * Le driver radio concret (Wio-E5, Core1262, …) est choisi par
 * CONFIG_MESHPAY_LORA_DRIVER dans Kconfig (composant device_hal).
 */

#include "transport_lora.h"

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_random.h"
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "app_state.h"
#include "comm/comm_msg.h"
#include "comm/lora_sync.h"
#include "hal/hal_lora.h"
#include "hal/hal_lora_factory.h"
#include "persistence/ledger_store.h"
#include "time_glue.h"
#include "transaction/tx_types.h"

static const char *TAG = "tport_lora";

/* ----------------------------------------------------------------
 * Constantes specifiques LoRa (anciennement dans app_state.h)
 * ---------------------------------------------------------------- */

/** Intervalle de sync LoRa (ms). */
#if CONFIG_MESHPAY_TEST_DEVICE_SEED
#define LORA_SYNC_INTERVAL_MS  15000
#else
#define LORA_SYNC_INTERVAL_MS  120000
#endif

/* ----------------------------------------------------------------
 * Etat prive : HAL, buffers, config lora_sync_task
 * ---------------------------------------------------------------- */

static hal_lora_t s_lora_hal;

/*
 * Flag de disponibilite LoRa.
 *
 * [F-LT-001] Avant ce flag, `transport_lora_available()` retournait
 * `true` inconditionnellement et `transport_lora_send()` appelait
 * `s_lora_hal.send(...)` sans verifier que la HAL avait ete cree avec
 * succes. Si `hal_lora_create_default()` echouait au boot (cf.
 * `transport_lora_init_and_start()` qui ne fait que logger un warning),
 * `s_lora_hal.send` restait NULL (BSS) et le premier appel a
 * `transport_lora_send` (typiquement l'attestation du premier paiement
 * recu) provoquait un crash NULL-deref (Guru Meditation
 * InstrFetchProhibited) sur les deux devices.
 *
 * `s_lora_ready` n'est passe a `true` que lorsque la HAL est creee ET
 * que `lora_sync_task` est spawne avec succes. La validation finale du
 * chip SX1262 (reset, check ID, IRQ) reste interne au driver via
 * `c1262_init`, qui tourne dans la task LoRa : la garde
 * `s_lora_hal.send != NULL` (assuree par `s_lora_ready`) suffit pour
 * eliminer le crash de pointeur.
 */
static bool s_lora_ready = false;

/*
 * Buffers de relay/pong (precedemment dans app_state.{h,c}).
 *
 * [F-MN-001] Tous les relais (broadcast, ping, pong) suivent désormais
 * le même pattern non-bloquant : enregistrement du tick de début + delay
 * cible à l'enregistrement, vérification par scrutation dans
 * `transport_lora_pump()`. Avant ce fix, broadcast et ping faisaient un
 * `vTaskDelay(200-1000 ms)` directement dans `pump()`, bloquant core_task
 * pendant ce délai et risquant la saturation de `s_evt_queue` (16).
 */
static uint8_t    s_relay_bcast_buf[COMM_MSG_LORA_MAX];
static size_t     s_relay_bcast_len = 0;
static bool       s_relay_bcast_pending = false;
static uint32_t   s_relay_bcast_delay_ms = 0;
static TickType_t s_relay_bcast_start_tick = 0;

static uint8_t    s_relay_ping_buf[COMM_MSG_PING_SIZE];
static size_t     s_relay_ping_len = 0;
static bool       s_relay_ping_pending = false;
static uint32_t   s_relay_ping_delay_ms = 0;
static TickType_t s_relay_ping_start_tick = 0;

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

static bool tx_id_already_collected(const transaction_t *txs,
                                    uint32_t count,
                                    const hash_t *id)
{
    if (txs == NULL || id == NULL) {
        return false;
    }
    for (uint32_t i = 0; i < count; i++) {
        if (hash_equal(&txs[i].id, id)) {
            return true;
        }
    }
    return false;
}

static int compare_tx_page_order(const transaction_t *a,
                                 const transaction_t *b)
{
    if (a->timestamp < b->timestamp) return -1;
    if (a->timestamp > b->timestamp) return 1;
    return memcmp(a->id.bytes, b->id.bytes, sizeof(a->id.bytes));
}

static void insert_lora_sync_candidate(transaction_t *out_buf,
                                       uint32_t *written,
                                       uint32_t max_count,
                                       const transaction_t *tx)
{
    if (!out_buf || !written || !tx || max_count == 0 ||
        tx_id_already_collected(out_buf, *written, &tx->id)) {
        return;
    }

    if (*written == max_count &&
        compare_tx_page_order(tx, &out_buf[max_count - 1]) >= 0) {
        return;
    }

    uint32_t pos = 0;
    while (pos < *written && compare_tx_page_order(&out_buf[pos], tx) <= 0) {
        pos++;
    }

    uint32_t limit = (*written < max_count) ? *written : max_count - 1;
    for (uint32_t i = limit; i > pos; i--) {
        out_buf[i] = out_buf[i - 1];
    }
    out_buf[pos] = *tx;
    if (*written < max_count) {
        (*written)++;
    }
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

    uint32_t written = 0;

    /*
     * Timeout 1 s (parallelisme avec l'ancien code) : si core_task
     * tient le mutex trop longtemps, on saute ce cycle ; le prochain
     * reessaiera.
     */
    if (xSemaphoreTake(s_state_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        ESP_LOGW(TAG, "Sync LoRa : mutex indisponible, cycle saute");
        *out_newest_ts = since_ts;
        return 0;
    }

#if CONFIG_MESHPAY_TEST_DEVICE_SEED
    transaction_t seed_tx;
    if (test_device_seed_get_tx(&seed_tx)) {
        insert_lora_sync_candidate(out_buf, &written, max_count, &seed_tx);
    }
#endif

    for (uint32_t i = 0; i < s_dag.count; i++) {
        const transaction_t *tx = &s_dag.transactions[i];
        if (tx->status == TX_STATUS_CONFIRMED && tx->timestamp > since_ts) {
#if CONFIG_MESHPAY_TEST_DEVICE_SEED
            if (memcmp(&tx->id, &seed_tx.id, sizeof(hash_t)) == 0) {
                continue;
            }
#endif
            insert_lora_sync_candidate(out_buf, &written, max_count, tx);
        }
    }

    xSemaphoreGive(s_state_mutex);

    /*
     * Apres reboot, les TX recentes confirmees peuvent etre consolidees
     * dans le checkpoint et donc absentes de s_dag. On les garde quand
     * meme dans la fenetre durable pour l'historique UI et pour le gossip
     * LoRa post-reboot : sans cette passe, un device redemarre ne propage
     * plus ses confirmations recentes.
     */
    {
        transaction_t *recent =
            (transaction_t *)malloc(sizeof(transaction_t) * max_count);
        if (recent != NULL) {
            uint32_t recent_count = 0;
            if (ledger_tx_window_read_recent(recent, max_count,
                                             &recent_count) == ESP_OK) {
                for (uint32_t i = 0; i < recent_count; i++) {
                    const transaction_t *tx = &recent[i];
                    if (tx->status != TX_STATUS_CONFIRMED ||
                        tx->timestamp <= since_ts) {
                        continue;
                    }
                    insert_lora_sync_candidate(out_buf, &written, max_count, tx);
                }
            }
            free(recent);
        }
    }

    *out_newest_ts = (written > 0 && out_buf[written - 1].timestamp > since_ts)
                         ? out_buf[written - 1].timestamp
                         : since_ts;
    return written;
}

static bool lora_get_dag_summary(lora_dag_summary_t *out_summary, void *ctx)
{
    (void)ctx;
    if (out_summary == NULL) {
        return false;
    }

    memset(out_summary, 0, sizeof(*out_summary));

    if (xSemaphoreTake(s_state_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        ESP_LOGW(TAG, "DAG_SUMMARY : mutex indisponible");
        return false;
    }

    out_summary->checkpoint_timestamp = s_checkpoint.timestamp;
    out_summary->last_tx_timestamp = s_checkpoint.timestamp;
    out_summary->tx_count_window = (s_dag.count > UINT16_MAX)
                                       ? UINT16_MAX
                                       : (uint16_t)s_dag.count;

    for (uint32_t i = 0; i < s_dag.count; i++) {
        const transaction_t *tx = &s_dag.transactions[i];
        if (tx->status == TX_STATUS_CONFIRMED &&
            tx->timestamp > out_summary->last_tx_timestamp) {
            out_summary->last_tx_timestamp = tx->timestamp;
        }
    }

    const transaction_t *tips[COMM_MSG_DAG_SUMMARY_MAX_TIPS];
    uint32_t tip_count = 0;
    if (dag_get_tips(&s_dag, tips, COMM_MSG_DAG_SUMMARY_MAX_TIPS,
                     &tip_count) == ESP_OK) {
        out_summary->tip_count = (uint8_t)tip_count;
        for (uint32_t i = 0; i < tip_count; i++) {
            out_summary->tips[i] = tips[i]->id;
        }
    }

    xSemaphoreGive(s_state_mutex);
    return true;
}

static uint32_t lora_collect_attestations(comm_msg_attestation_t *out_buf,
                                          uint32_t max_count,
                                          void *ctx)
{
    (void)ctx;
    uint32_t count = 0;
    if (out_buf == NULL || max_count == 0) {
        return 0;
    }
    if (ledger_attestation_window_read_recent(out_buf, max_count, &count) != ESP_OK) {
        return 0;
    }
    return count;
}

/* ----------------------------------------------------------------
 * API publique
 * ---------------------------------------------------------------- */

bool transport_lora_available(void)
{
    /*
     * [F-LT-001] Reflete l'etat reel du sous-systeme LoRa. Auparavant
     * retournait `true` en dur, ce qui faisait croire a tous les
     * callers que la HAL etait initialisee meme si `create()` avait
     * echoue. Maintenant les callers (handler_payment, etc.) peuvent
     * fiablement skipper le LoRa quand il n'est pas dispo.
     */
    return s_lora_ready;
}

hal_err_t transport_lora_init_and_start(void)
{
    /* 1. HAL physique — le driver concret est choisi par Kconfig. */
    hal_err_t err = hal_lora_create_default(&s_lora_hal);
    if (err != HAL_OK) {
        ESP_LOGE(TAG, "HAL LoRa create echoue (%d) — LoRa desactive, "
                      "attestations et sync periodique skipees", err);
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
    s_lora_cfg.get_dag_summary        = lora_get_dag_summary;
    s_lora_cfg.collect_attestations    = lora_collect_attestations;
    s_lora_cfg.collect_ctx            = NULL;
    s_lora_cfg.sync_interval_ms       = LORA_SYNC_INTERVAL_MS;
    s_lora_cfg.is_master              = false;
    s_lora_cfg.get_time               = get_time_ms_wrapper;
    s_lora_cfg.own_pubkey             = &s_keypair.public_key;
    s_lora_cfg.own_keypair            = &s_keypair;
    s_lora_cfg.get_lamport            = lora_get_lamport;

    BaseType_t ok = xTaskCreate(lora_sync_task, "lora", LORA_TASK_STACK,
                                &s_lora_cfg, LORA_TASK_PRIO, NULL);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreate(lora_sync_task) echoue — LoRa desactive");
        return HAL_ERR_NO_MEM;
    }

    /*
     * [F-LT-001] La HAL a ete cree et la task est spawnee : le pointeur
     * `s_lora_hal.send` est garanti non-NULL (set par
     * hal_lora_core1262_create). Le chip lui-meme sera initialise par
     * `c1262_init` dans la task LoRa ; tant que `s_ctx.initialized`
     * n'est pas vrai, `c1262_send` retournera HAL_FAIL proprement
     * (cf. hal_lora_core1262.c:449), donc pas de crash.
     */
    s_lora_ready = true;
    return HAL_OK;
}

bool transport_lora_send(const uint8_t *buf, size_t len, const char *what)
{
    /*
     * [F-LT-001] Garde anti-NULL-deref : si `s_lora_ready` est faux,
     * `s_lora_hal.send` peut etre NULL (echec de create()) et l'appel
     * provoquerait un crash. On loggue en DEBUG seulement pour eviter
     * de spammer les logs en cas de paiement repete sur device sans
     * LoRa fonctionnel.
     */
    if (!s_lora_ready) {
        ESP_LOGD(TAG, "send (%s) skip : LoRa non pret", what ? what : "?");
        return false;
    }
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
    /*
     * [F-MN-001] Délai anti-collision (200-1000 ms) calculé à
     * l'enregistrement. La scrutation dans `transport_lora_pump()`
     * envoie quand le délai est écoulé, sans bloquer core_task.
     */
    s_relay_bcast_delay_ms = 200 + (esp_random() % 801);
    s_relay_bcast_start_tick = xTaskGetTickCount();
    s_relay_bcast_pending = true;
}

void transport_lora_queue_relay_ping(const uint8_t *buf, size_t len)
{
    if (len > sizeof(s_relay_ping_buf)) return;
    memcpy(s_relay_ping_buf, buf, len);
    s_relay_ping_len = len;
    /* [F-MN-001] Pattern identique au broadcast. */
    s_relay_ping_delay_ms = 200 + (esp_random() % 801);
    s_relay_ping_start_tick = xTaskGetTickCount();
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
     * [F-LT-001] Si la HAL LoRa n'a pas pu etre initialisee, pas la
     * peine de scruter les buffers : ils ne se rempliront pas (les
     * callers utilisent transport_lora_available() en garde), et meme
     * s'ils etaient remplis, transport_lora_send() les rejetterait.
     */
    if (!s_lora_ready) {
        return;
    }

    /*
     * [F-MN-001] Relay broadcast LoRa — scrutation non-bloquante.
     * Le délai anti-collision (200-1000 ms) a été enregistré à l'appel
     * de `transport_lora_queue_relay_broadcast`. On envoie quand le
     * délai est écoulé, sans bloquer core_task.
     */
    if (s_relay_bcast_pending) {
        TickType_t elapsed = xTaskGetTickCount() - s_relay_bcast_start_tick;
        if (elapsed >= pdMS_TO_TICKS(s_relay_bcast_delay_ms)) {
            s_relay_bcast_pending = false;
            if (transport_lora_send(s_relay_bcast_buf, s_relay_bcast_len, "relay-bcast")) {
                ESP_LOGI(TAG, "Broadcast relaye (delai=%lums)",
                         (unsigned long)s_relay_bcast_delay_ms);
            }
        }
    }

    /* [F-MN-001] Relay ping LoRa — même pattern non-bloquant. */
    if (s_relay_ping_pending) {
        TickType_t elapsed = xTaskGetTickCount() - s_relay_ping_start_tick;
        if (elapsed >= pdMS_TO_TICKS(s_relay_ping_delay_ms)) {
            s_relay_ping_pending = false;
            if (transport_lora_send(s_relay_ping_buf, s_relay_ping_len, "relay-ping")) {
                ESP_LOGI(TAG, "PING relaye (delai=%lums)",
                         (unsigned long)s_relay_ping_delay_ms);
            }
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

void transport_lora_set_sync_interval(uint32_t interval_ms)
{
    /* Memorise la nouvelle valeur dans la config de lora_sync_task.
     * lora_sync_task relit sync_interval_ms a chaque cycle (cf.
     * lora_sync.c : "Dormir sync_interval_ms"), donc la prise en
     * compte est effective au cycle suivant. */
    s_lora_cfg.sync_interval_ms = interval_ms;
}
