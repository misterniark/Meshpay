/**
 * @file main.c
 * @brief Point d'entree du systeme de paiement decentralise offline.
 *
 * Orchestre tous les composants : crypto, DAG, wallet, lock_table,
 * time_manager, currency, ESP-NOW et LoRa.
 *
 * Architecture FreeRTOS :
 * - espnow_task (P7) : reception/envoi ESP-NOW
 * - core_task   (P6) : boucle d'evenements centrale
 * - lora_task   (P5) : cycles LoRa sync periodiques
 *
 * Communication inter-taches :
 * - evt_queue : espnow/lora -> core_task (evenements entrants)
 * - cmd_queue : core_task -> espnow_task (commandes sortantes)
 * - state_mutex : protege DAG + wallet + lock_table
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ESP-IDF */
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#include "esp_random.h"

/*
 * Init NVS : la complexite du chiffrement (CONFIG_NVS_ENCRYPTION) est
 * isolee dans app_init/nvs_init_{secure,plain}.c (Lot D.8). main.c
 * appelle nvs_init_storage() sans aucun `#if`.
 */
#include "app_init/nvs_init.h"

/*
 * Etat global et capabilites (MP_HAS_ESPNOW) :
 * voir app_state.h.
 *
 * Modules extraits au Lot D :
 *   - app_state            : storage des variables d'etat
 *   - time_glue            : wrappers temps + collect TX confirmees
 *   - stack_monitor        : tache de log des HWM
 *   - peers                : add_peer / find_peer_mac
 *   - currency_config_init : init de s_currency
 */
#include "app_state.h"
#include "time_glue.h"
#include "stack_monitor.h"
#include "peers.h"
#include "currency_config_init.h"
#include "persistence/ledger_store.h"

/* Core */
#include "crypto/crypto_init.h"
#include "crypto/crypto_keys.h"
#include "crypto/crypto_sign.h"
#include "transaction/tx_create.h"
#include "transaction/tx_validate.h"
#include "transaction/tx_serialize.h"
#include "dag/dag_merge.h"
#include "dag/dag_prune.h"

/* Currency */
#include "currency/currency_rules.h"
#include "currency/currency_melt.h"

/* Debug console (stub si CONFIG_MESHPAY_DEBUG_CONSOLE=n — voir le
 * header pour la strategie de gating). */
#include "debug_console/debug_console.h"

static const char *TAG = "main";

/* Constantes (queues, stacks, priorites, cles NVS, MAX_PEERS, etc.)
 * deplacees dans app_state.h (Lot D 2026-05-13). */

/* ================================================================
 * Declarations des factories HAL (pas de header public)
 * ================================================================ */

extern hal_err_t hal_storage_esp32_create(hal_storage_t *storage);

extern hal_err_t espnow_hal_esp32_create(espnow_hal_t *hal);
/* HAL LoRa factory : declaration deplacee dans transport/transport_lora.c
 * (Lot D.3). Le code applicatif passe par transport_lora_init_and_start. */
#if CONFIG_IDF_TARGET_ESP32
extern hal_err_t hal_display_ili9341_create(hal_display_t *display);
#elif CONFIG_IDF_TARGET_ESP32S3
extern hal_err_t hal_display_jd9853_create(hal_display_t *display);
#endif

/** Tache UI (definie dans ui_task.c) */
extern void ui_task(void *pvParam);

/* Etat global et fonctions wrappers temps deplaces dans app_state.{h,c}
 * et time_glue.{h,c} au Lot D (2026-05-13). */

/* Debug console : 4 callbacks de dump JSON deplaces dans
 * debug_console_dumps.c (Lot D.7). Le fichier .c est selectionne par
 * CMake : impl reelle si CONFIG_MESHPAY_DEBUG_CONSOLE=y, sinon stub.
 * L appel a debug_console_register_dumps() en app_main est inconditionnel. */
#include "debug_console_dumps.h"

/* Stack monitoring deplace dans stack_monitor.{h,c} au Lot D. */

/* Persistance NVS deplacee dans main/persistence/ au Lot D.2 :
 *   - nvs_keypair    : load_or_generate_keypair
 *   - nvs_checkpoint : nvs_checkpoint_load / nvs_checkpoint_save
 *   - nvs_alias      : load_or_generate_alias + generate_random_alias
 *   - nvs_next_seq   : next_seq + load_next_seq_or_recompute
 *   - nvs_beneficiary: load beneficiaire au boot
 *
 * Les pointeurs s_checkpoint_save / s_checkpoint_load sont desormais
 * declares dans app_state.h et initialises en app_main.
 */
#include "persistence/nvs_keypair.h"
#include "persistence/nvs_checkpoint.h"
#include "persistence/nvs_alias.h"
#include "persistence/nvs_next_seq.h"
#include "persistence/nvs_beneficiary.h"

/* Facade LoRa (Lot D.3) — voir transport/transport_lora.h pour le pourquoi. */
#include "transport/transport_lora.h"
/* Solde, DAG insert, handlers, ops, core_task et UI dispatch deplaces
 * dans main/ au Lot D.6 :
 *   - balance.{h,c}        : compute_owner_balance, ui_get_owner_balance
 *   - dag_glue.{h,c}       : dag_insert_and_track, auto_checkpoint_if_needed
 *   - handlers/handlers.h  : 11 handlers d'evenements (Lot D.4)
 *   - ops/ops.h            : 7 operations applicatives (Lot D.5)
 *   - core_task.{h,c}      : boucle FreeRTOS centrale
 *   - ui_dispatch.{h,c}    : handle_ui_command
 */
#include "balance.h"
#include "dag_glue.h"
#include "handlers/handlers.h"
#include "ops/ops.h"
#include "core_task.h"
#include "ui_dispatch.h"
#include "power_manager.h"
#include "hal/hal_power.h"
#include "esp_pm.h"
#include "sdkconfig.h"

#if CONFIG_MESHPAY_TEST_DEVICE_SEED && CONFIG_SECURE_FLASH_ENCRYPTION_MODE_RELEASE
#error "MESHPAY_TEST_DEVICE_SEED interdit en mode RELEASE — desactiver dans menuconfig."
#endif

/* ================================================================
 * Gestion de l'energie (feature 13) — adaptateurs pour power_manager
 * ================================================================ */

/* Frequences CPU (MHz) pilotees par esp_pm selon l'etat d'energie.
 * Definies ici (et non dans power_manager.c) car power_manager.c est
 * dependance-pure : il ne connait pas esp_pm. */
#define POWER_MAX_FREQ_MHZ        240
#define POWER_ACTIF_MIN_FREQ_MHZ  240   /* ACTIF : pas de scaling */
#define POWER_ECO_MIN_FREQ_MHZ     80   /* ECO   : scaling autorise */

/* HAL source d'alimentation (stub : toujours USB tant que le hardware
 * batterie n'existe pas). */
static hal_power_t s_power_hal;

/* Mutex dedie au power_manager : protege sa machine d'etats entre
 * core_task (tick) et ui_task (notify_activity). Dedie pour ne pas
 * interferer avec s_state_mutex. */
static SemaphoreHandle_t s_power_mutex;

/* Adaptateur get_power_source : power_manager attend une fonction sans
 * argument, hal_power expose une vtable avec ctx. */
static hal_power_source_t power_get_source_adapter(void)
{
    return s_power_hal.get_source(s_power_hal.ctx);
}

/* Adaptateur set_backlight : delegue au HAL display. */
static void power_set_backlight_adapter(uint8_t pct)
{
    if (s_display.set_backlight != NULL) {
        s_display.set_backlight(pct, s_display.ctx);
    }
}

/* Adaptateur apply_pm_config : configure esp_pm selon l'etat.
 * Phase 1 : frequency scaling seul, light_sleep_enable reste false. */
static void power_apply_pm_config(power_state_t state)
{
    esp_pm_config_t pm = {
        .max_freq_mhz       = POWER_MAX_FREQ_MHZ,
        .min_freq_mhz       = (state == POWER_STATE_ECO)
                                  ? POWER_ECO_MIN_FREQ_MHZ
                                  : POWER_ACTIF_MIN_FREQ_MHZ,
        .light_sleep_enable = false,   /* Phase 1 : pas de light sleep */
    };
    esp_err_t err = esp_pm_configure(&pm);
    if (err != ESP_OK) {
        /* Sur le CYD, CONFIG_PM_ENABLE n'est pas active : esp_pm_configure
         * renvoie ESP_ERR_NOT_SUPPORTED. C'est attendu et sans consequence
         * (le power_manager du CYD est le stub, il n'appelle jamais ceci ;
         * seul l'appel de boot ci-dessous le declenche). */
        ESP_LOGD(TAG, "esp_pm_configure: 0x%x (attendu si PM desactive)", err);
    }
}

/* Adaptateurs lock/unlock : mutex dedie au power_manager. */
static void power_lock(void)   { xSemaphoreTake(s_power_mutex, portMAX_DELAY); }
static void power_unlock(void) { xSemaphoreGive(s_power_mutex); }

static bool dag_integrity_parent_exists_unlocked(const hash_t *id)
{
    for (uint32_t i = 0; i < s_dag.count; i++) {
        if (hash_equal(&s_dag.transactions[i].id, id)) {
            return true;
        }
    }
    return false;
}

static bool dag_integrity_parent_covered_by_checkpoint(const hash_t *id,
                                                       uint64_t checkpoint_ts)
{
    if (id == NULL || checkpoint_ts == 0 || hash_is_zero(id)) {
        return false;
    }

    if (!hash_is_zero(&s_checkpoint.last_tx_id) &&
        hash_equal(id, &s_checkpoint.last_tx_id)) {
        return true;
    }

    enum { RECENT_SCAN_MAX = 128 };
    transaction_t *recent =
        (transaction_t *)malloc(sizeof(transaction_t) * RECENT_SCAN_MAX);
    if (recent == NULL) {
        return false;
    }

    uint32_t count = 0;
    bool covered = false;
    if (ledger_tx_window_read_recent(recent, RECENT_SCAN_MAX, &count) == ESP_OK) {
        for (uint32_t i = 0; i < count; i++) {
            const transaction_t *tx = &recent[i];
            if (tx->status == TX_STATUS_CONFIRMED &&
                tx->timestamp <= checkpoint_ts &&
                hash_equal(&tx->id, id)) {
                covered = true;
                break;
            }
        }
    }

    free(recent);
    return covered;
}

static void dag_integrity_log(const char *context)
{
    uint32_t errors = 0;
    uint32_t locked = 0;
    uint32_t confirmed = 0;
    uint32_t cancelled = 0;
    uint32_t missing_pre_checkpoint = 0;
    const uint64_t checkpoint_ts = s_checkpoint.timestamp;

    master_keys_t masters = {
        .keys = s_currency.mint_authorities,
        .count = s_currency.mint_authority_count,
    };

    if (s_dag.count > DAG_MAX_TRANSACTIONS) {
        ESP_LOGE(TAG, "DAG integrity[%s]: count invalide %"PRIu32"/%u",
                 context, s_dag.count, DAG_MAX_TRANSACTIONS);
        errors++;
    }

    for (uint32_t i = 0; i < s_dag.count; i++) {
        const transaction_t *tx = &s_dag.transactions[i];

        switch (tx->status) {
            case TX_STATUS_LOCKED:    locked++; break;
            case TX_STATUS_CONFIRMED: confirmed++; break;
            case TX_STATUS_CANCELLED: cancelled++; break;
            default:
                ESP_LOGE(TAG, "DAG integrity[%s]: tx[%"PRIu32"] statut invalide=%d",
                         context, i, (int)tx->status);
                errors++;
                break;
        }

        if (tx_validate_structure(tx) != ESP_OK) {
            ESP_LOGE(TAG, "DAG integrity[%s]: tx[%"PRIu32"] structure invalide",
                     context, i);
            errors++;
        }
        if (tx_validate_signature(tx) != ESP_OK) {
            ESP_LOGE(TAG, "DAG integrity[%s]: tx[%"PRIu32"] hash/signature invalide",
                     context, i);
            errors++;
        }
        if (tx_validate_master(tx, &masters) != ESP_OK) {
            ESP_LOGE(TAG, "DAG integrity[%s]: tx[%"PRIu32"] autorite MINT invalide",
                     context, i);
            errors++;
        }

        for (uint32_t j = i + 1; j < s_dag.count; j++) {
            const transaction_t *other = &s_dag.transactions[j];
            if (hash_equal(&tx->id, &other->id)) {
                ESP_LOGE(TAG, "DAG integrity[%s]: doublon id tx[%"PRIu32"] tx[%"PRIu32"]",
                         context, i, j);
                errors++;
            }
            if (public_key_equal(&tx->from, &other->from) &&
                tx->seq == other->seq &&
                !hash_equal(&tx->id, &other->id) &&
                tx->status != TX_STATUS_CANCELLED &&
                other->status != TX_STATUS_CANCELLED) {
                ESP_LOGE(TAG, "DAG integrity[%s]: conflit seq actif tx[%"PRIu32"] tx[%"PRIu32"] seq=%"PRIu32,
                         context, i, j, tx->seq);
                errors++;
            }
        }

        if (tx->parent_count == 2 &&
            hash_equal(&tx->parents[0], &tx->parents[1])) {
            ESP_LOGE(TAG, "DAG integrity[%s]: tx[%"PRIu32"] parents dupliques",
                     context, i);
            errors++;
        }

        for (uint8_t p = 0; p < tx->parent_count && p < TX_MAX_PARENTS; p++) {
            const hash_t *parent = &tx->parents[p];
            if (hash_is_zero(parent)) {
                if (tx->type != TX_TYPE_MINT) {
                    ESP_LOGE(TAG, "DAG integrity[%s]: tx[%"PRIu32"] parent zero hors MINT",
                             context, i);
                    errors++;
                }
                continue;
            }
            if (hash_equal(&tx->id, parent)) {
                ESP_LOGE(TAG, "DAG integrity[%s]: tx[%"PRIu32"] self-parent",
                         context, i);
                errors++;
            }
            if (!dag_integrity_parent_exists_unlocked(parent)) {
                if (dag_integrity_parent_covered_by_checkpoint(parent,
                                                               checkpoint_ts)) {
                    missing_pre_checkpoint++;
                } else {
                    ESP_LOGE(TAG, "DAG integrity[%s]: tx[%"PRIu32"] parent absent",
                             context, i);
                    errors++;
                }
            }
        }
    }

    const transaction_t *tips[DAG_MAX_TIPS];
    uint32_t tip_count = 0;
    uint32_t total_tips = 0;
    if (dag_get_tips_ext(&s_dag, tips, DAG_MAX_TIPS, &tip_count, &total_tips) != ESP_OK) {
        ESP_LOGE(TAG, "DAG integrity[%s]: calcul tips echoue", context);
        errors++;
    }

    ESP_LOGI(TAG, "DAG integrity[%s]: %s count=%"PRIu32" tips=%"PRIu32
                  " locked=%"PRIu32" confirmed=%"PRIu32" cancelled=%"PRIu32
                  " pre_checkpoint_parents=%"PRIu32,
             context,
             errors == 0 ? "OK" : "FAIL",
             s_dag.count,
             total_tips,
             locked,
             confirmed,
             cancelled,
             missing_pre_checkpoint);
}

static uint32_t ui_load_history_txs(transaction_t *out_txs, uint32_t max_count)
{
    uint32_t count = 0;
    if (ledger_tx_window_read_recent(out_txs, max_count, &count) != ESP_OK) {
        return 0;
    }
    return count;
}

static bool start_power_and_ui_task(void)
{
    /* ---- Gestion de l'energie (feature 13) ---- */
    s_power_mutex = xSemaphoreCreateMutex();
    if (s_power_mutex == NULL) {
        ESP_LOGE(TAG, "Erreur creation s_power_mutex");
        return false;
    }

    /* Config esp_pm de boot : etat ACTIF (pas de scaling). */
    power_apply_pm_config(POWER_STATE_ACTIF);

    hal_power_stub_create(&s_power_hal);

    power_manager_config_t power_cfg = {
        .get_time_ms      = get_time_ms_wrapper,
        .get_power_source = power_get_source_adapter,
        .set_backlight    = power_set_backlight_adapter,
        .apply_pm_config  = power_apply_pm_config,
        .lock             = power_lock,
        .unlock           = power_unlock,
        .eco_timeout_ms   = POWER_ECO_TIMEOUT_MS,
    };
    power_manager_init(&power_cfg);
    ESP_LOGI(TAG, "Gestion energie initialisee (etat ACTIF, source=%s)",
             power_get_source_adapter() == POWER_SOURCE_USB ? "USB" : "batterie");

    /*
     * L'UI doit demarrer avant ESP-NOW/LoRa/core sur ESP32-S3 Waveshare :
     * ces transports consomment assez de RAM interne pour faire echouer
     * xTaskCreate(ui_task) sur certains boots, ce qui laisse un ecran noir
     * alors que le firmware continue.
     */
    s_ui_cmd_queue = xQueueCreate(UI_CMD_QUEUE_DEPTH, sizeof(ui_cmd_t));
    if (!s_ui_cmd_queue) {
        ESP_LOGE(TAG, "Erreur creation ui_cmd_queue");
        return false;
    }

    memset(&s_ui_ctx, 0, sizeof(s_ui_ctx));
    s_ui_ctx.display             = &s_display;
    s_ui_ctx.storage             = &s_storage;
    s_ui_ctx.state_mutex         = s_state_mutex;
    s_ui_ctx.cmd_queue           = s_ui_cmd_queue;
    s_ui_ctx.wallet              = &s_wallet;
    s_ui_ctx.dag                 = &s_dag;
    s_ui_ctx.currency            = &s_currency;
    s_ui_ctx.own_pubkey          = &s_keypair.public_key;
    s_ui_ctx.device_alias        = s_device_alias;
    s_ui_ctx.device_alias_len    = &s_device_alias_len;
    s_ui_ctx.broadcast_pending   = &s_broadcast_pending;
    s_ui_ctx.pending_broadcast   = &s_pending_broadcast;
    s_ui_ctx.peers               = s_peers;
    s_ui_ctx.peer_count          = &s_peer_count;
    s_ui_ctx.ping_results        = (ui_ping_result_t *)s_ping_results;
    s_ui_ctx.ping_result_count   = &s_ping_result_count;
    s_ui_ctx.beneficiary_key     = &s_beneficiary_key;
    s_ui_ctx.forward_interval_min = &s_forward_interval_min;
    s_ui_ctx.pay_feedback        = &s_pay_feedback;

    /* Determiner si ce device est un maitre (present dans mint_authorities).
     * Cette information est utilisee par l'UI pour autoriser l'acces admin. */
    s_ui_ctx.compute_melted_balance = compute_melted_balance;
    s_ui_ctx.get_owner_balance      = ui_get_owner_balance;
    s_ui_ctx.load_history_txs       = ui_load_history_txs;
    s_ui_ctx.notify_activity        = power_manager_notify_activity;

    s_ui_ctx.is_master = false;
    for (uint32_t i = 0; i < s_currency.mint_authority_count; i++) {
        if (public_key_equal(&s_keypair.public_key,
                             &s_currency.mint_authorities[i])) {
            s_ui_ctx.is_master = true;
            break;
        }
    }

    BaseType_t task_ok = xTaskCreate(ui_task, "ui", UI_TASK_STACK, &s_ui_ctx,
                                     UI_TASK_PRIO, NULL);
    if (task_ok != pdPASS) {
        ESP_LOGE(TAG, "Erreur creation tache ui");
        return false;
    }

    return true;
}

#if CONFIG_MESHPAY_TEST_DEVICE_SEED
static bool test_device_seed_already_applied(void)
{
    char stored_id[64] = {0};
    size_t stored_len = sizeof(stored_id);
    hal_err_t herr = s_storage.blob_read(NVS_NAMESPACE,
                                         NVS_KEY_TEST_SEED_ID,
                                         (uint8_t *)stored_id,
                                         &stored_len,
                                         s_storage.ctx);
    if (herr != HAL_OK || stored_len == 0) {
        return false;
    }

    const char *current_id = CONFIG_MESHPAY_TEST_DEVICE_SEED_ID;
    size_t current_len = strlen(current_id) + 1;
    return stored_len == current_len &&
           memcmp(stored_id, current_id, current_len) == 0;
}

static esp_err_t save_test_device_seed_tx(const transaction_t *tx)
{
    if (tx == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    hal_err_t herr = s_storage.blob_write(NVS_NAMESPACE,
                                          NVS_KEY_TEST_SEED_TX,
                                          (const uint8_t *)tx,
                                          sizeof(transaction_t),
                                          s_storage.ctx);
    return (herr == HAL_OK) ? ESP_OK : ESP_FAIL;
}

bool test_device_seed_get_tx(transaction_t *out_tx)
{
    if (out_tx == NULL) {
        return false;
    }

    memset(out_tx, 0, sizeof(*out_tx));

    if (!test_device_seed_already_applied()) {
        return false;
    }

    size_t stored_len = sizeof(*out_tx);
    hal_err_t herr = s_storage.blob_read(NVS_NAMESPACE,
                                         NVS_KEY_TEST_SEED_TX,
                                         (uint8_t *)out_tx,
                                         &stored_len,
                                         s_storage.ctx);
    if (herr == HAL_OK && stored_len == sizeof(*out_tx) &&
        out_tx->type == TX_TYPE_MINT &&
        out_tx->status == TX_STATUS_CONFIRMED &&
        out_tx->amount == CONFIG_MESHPAY_TEST_DEVICE_SEED_AMOUNT &&
        public_key_equal(&out_tx->from, &s_keypair.public_key) &&
        public_key_equal(&out_tx->to, &s_keypair.public_key) &&
        tx_validate_signature(out_tx) == ESP_OK) {
        return true;
    }

    /*
     * Migration des devices deja seedes avant la persistance de la TX de
     * preuve : reconstruire une self-MINT signee, la stocker, puis la
     * diffuser aux peers. Elle n'est pas inseree dans le DAG local pour ne
     * pas recompter le checkpoint de seed deja consolide.
     */
    hash_t parent;
    memset(&parent, 0, sizeof(parent));
    uint64_t timestamp = s_checkpoint.timestamp > 0
                             ? s_checkpoint.timestamp
                             : get_tx_timestamp_wrapper();
    esp_err_t ret = tx_create_mint(out_tx,
                                   &s_keypair,
                                   &s_keypair.public_key,
                                   CONFIG_MESHPAY_TEST_DEVICE_SEED_AMOUNT,
                                   s_currency.currency_id,
                                   next_seq(),
                                   &parent,
                                   1,
                                   timestamp);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Seed test: reconstruction TX preuve echouee (%d)", ret);
        return false;
    }

    ret = save_test_device_seed_tx(out_tx);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Seed test: sauvegarde TX preuve echouee");
        return false;
    }

    ESP_LOGW(TAG, "Seed test: TX preuve reconstruite pour propagation peers");
    return true;
}

static esp_err_t run_test_device_seed(void)
{
    if (test_device_seed_already_applied()) {
        ESP_LOGI(TAG, "[10b/12] Seed test deja applique (id=\"%s\")",
                 CONFIG_MESHPAY_TEST_DEVICE_SEED_ID);
        return ESP_OK;
    }

    hash_t parents[DAG_MAX_TIPS];
    uint32_t parent_count = 0;
    uint32_t total_tips = 0;
    const transaction_t *tips[DAG_MAX_TIPS];

    if (dag_get_tips_ext(&s_dag, tips, DAG_MAX_TIPS,
                         &parent_count, &total_tips) == ESP_OK &&
        parent_count > 0) {
        for (uint32_t i = 0; i < parent_count; i++) {
            parents[i] = tips[i]->id;
        }
    } else if (s_checkpoint.timestamp > 0 &&
               !hash_is_zero(&s_checkpoint.last_tx_id)) {
        parents[0] = s_checkpoint.last_tx_id;
        parent_count = 1;
    } else {
        memset(&parents[0], 0, sizeof(hash_t));
        parent_count = 1;
    }

    transaction_t mint_tx;
    esp_err_t ret = tx_create_mint(&mint_tx,
                                   &s_keypair,
                                   &s_keypair.public_key,
                                   CONFIG_MESHPAY_TEST_DEVICE_SEED_AMOUNT,
                                   s_currency.currency_id,
                                   next_seq(),
                                   parents,
                                   parent_count,
                                   get_tx_timestamp_wrapper());
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "[10b/12] Erreur creation seed test: %d", ret);
        return ret;
    }

    ret = dag_insert_and_track(&mint_tx);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "[10b/12] Erreur insertion seed test: %d", ret);
        return ret;
    }

    ret = save_test_device_seed_tx(&mint_tx);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "[10b/12] Erreur persistance TX seed test");
        return ret;
    }

    persist_runtime_checkpoint("test_device_seed");

    const char *seed_id = CONFIG_MESHPAY_TEST_DEVICE_SEED_ID;
    hal_err_t herr = s_storage.blob_write(NVS_NAMESPACE,
                                          NVS_KEY_TEST_SEED_ID,
                                          (const uint8_t *)seed_id,
                                          strlen(seed_id) + 1,
                                          s_storage.ctx);
    if (herr != HAL_OK) {
        ESP_LOGE(TAG, "[10b/12] Erreur persistance seed id: %d", herr);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "[10b/12] Seed test applique: %"PRIu32" coin(s), id=\"%s\"",
             (uint32_t)CONFIG_MESHPAY_TEST_DEVICE_SEED_AMOUNT,
             CONFIG_MESHPAY_TEST_DEVICE_SEED_ID);
    return ESP_OK;
}
#endif

/* ================================================================
 * Point d'entree
 * ================================================================ */

void app_main(void)
{
    ESP_LOGI(TAG, "=== Offline Payment System - Demarrage ===");

    /* ---- 1. NVS (chiffre ou clair selon CONFIG_NVS_ENCRYPTION) ---- */
    esp_err_t ret;
    bool nvs_encrypted = false;
    ret = nvs_init_storage(&nvs_encrypted);
    if (ret != ESP_OK) {
        return;
    }
    ESP_LOGI(TAG, "[1/12] NVS initialise%s",
             nvs_encrypted ? " (chiffre)" : "");

    /* ---- 2. Storage HAL ---- */
    if (hal_storage_esp32_create(&s_storage) != HAL_OK) {
        ESP_LOGE(TAG, "Storage HAL init echoue");
        return;
    }
    ESP_LOGI(TAG, "[2/12] Storage HAL initialise");

    /* ---- 2bis. Initialisation centralisee du sous-systeme crypto ----
     *
     * Historique :
     *  - C6 (audit Sonnet, avril 2026) a introduit `crypto_init()` pour
     *    centraliser l'init PSA Crypto et eviter une race condition. Mais
     *    l'appel etait reste oublie ici : le smoke test du 2026-05-12 sur
     *    Waveshare ESP32-S3 a revele que `crypto_generate_keypair()`
     *    retournait `ESP_ERR_INVALID_STATE` (259) car la garde
     *    `crypto_is_initialized()` rejetait tout appel.
     *  - Lot E.2 (mai 2026) a retire la dependance PSA Crypto au profit de
     *    Monocypher (vendore), car mbedTLS IDF v5.4.3 ne fournit pas de
     *    driver Ed25519. `crypto_init()` est desormais un no-op qui se
     *    contente de basculer un flag, mais l'invariant C6 reste : aucun
     *    appel crypto ne doit precéder cet init.
     *
     * `crypto_init()` est idempotente et thread-safe (mutex statique
     * FreeRTOS) : elle peut etre appelee sans risque meme si des tests
     * automatises l'avaient deja invoquee.
     */
    ret = crypto_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Crypto init echoue: 0x%x", ret);
        return;
    }
    ESP_LOGI(TAG, "[2bis/12] Sous-systeme crypto initialise (Monocypher Ed25519)");

    bool keypair_existed = false;
    (void)s_storage.exists(NVS_NAMESPACE, NVS_KEY_PRIVKEY,
                           &keypair_existed, s_storage.ctx);

    /* ---- 3. Keypair ---- */
    ret = nvs_keypair_load_or_generate();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Keypair init echoue");
        return;
    }
    ESP_LOGI(TAG, "[3/12] Keypair pret");

    /* ---- 3b. Alias du device ---- */
    ret = nvs_alias_load_or_generate();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Alias init echoue, utilisation du defaut");
    }

    /* ---- 3c. Configuration beneficiaire (auto-forward) ---- */
    nvs_beneficiary_load();

    /* Init des callbacks de persistance checkpoint (backend storage).
     * Pour un backend mock (test), reassigner avant l'appel au load. */
    s_checkpoint_save = ledger_checkpoint_save;
    s_checkpoint_load = ledger_checkpoint_load;

    /* ---- 4. DAG ---- */
    dag_init(&s_dag);
    ESP_LOGI(TAG, "[4/12] DAG initialise");

    /* ---- 5. Checkpoint ---- */
    bool checkpoint_missing = (s_checkpoint_load(&s_checkpoint, NULL) != ESP_OK);
    bool first_boot = checkpoint_missing && !keypair_existed;
    if (first_boot) {
        memset(&s_checkpoint, 0, sizeof(checkpoint_t));
        ESP_LOGI(TAG, "[5/12] Premier demarrage, pas de checkpoint");
    } else if (checkpoint_missing) {
        memset(&s_checkpoint, 0, sizeof(checkpoint_t));
        ESP_LOGW(TAG, "[5/12] Checkpoint absent mais keypair deja present : "
                      "pas de MINT initial, attente du gossip peers");
    } else {
        ESP_LOGI(TAG, "[5/12] Checkpoint charge");
    }

    (void)ledger_tx_window_load_into_dag();
    uint32_t replayed_attestations = 0;
    if (ledger_attestation_apply_to_dag(&replayed_attestations) == ESP_OK &&
        replayed_attestations > 0) {
        persist_runtime_checkpoint("boot_attestation_replay");
    }

    /* [I3-fix] Charger le compteur s_next_seq pour eviter les reutilisations
     * de seq apres reboot. Appelle APRES chargement du checkpoint + DAG
     * (si NVS vide, reconstruit a partir du max des seq observes). */
    load_next_seq_or_recompute();

    /* ---- 6. Time manager ---- */
    time_manager_config_t tm_config = {
        .mode = TIME_MODE_LAMPORT,
        .get_monotonic = platform_get_monotonic_ms,
    };
    if (time_manager_init(&s_time_manager, &tm_config) != 0) {
        ESP_LOGE(TAG, "Time manager init echoue");
        return;
    }
    ESP_LOGI(TAG, "[6/12] Time manager initialise (mode Lamport)");

    /* ---- 7. Currency ---- */
    /*
     * [C6-fix] La configuration currency doit etre initialisee AVANT le
     * wallet : le wallet en extrait le fee_recipient (premier
     * mint_authority). Si currency est initialisee apres, s_currency est
     * encore zero lors du wallet_init → fee_recipient reste nul → tous
     * les fees sont brules au lieu d'etre rediriges vers l'organisateur.
     */
    init_currency_config();
    ESP_LOGI(TAG, "[7/12] Currency configuree (id=0x%08"PRIx32" \"%s\")",
             s_currency.currency_id, s_currency.name);

    /* ---- 8. Wallet ---- */
    ret = wallet_init(&s_wallet, &s_keypair.public_key,
                      &s_dag, get_time_ms_wrapper);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Wallet init echoue: %d", ret);
        return;
    }
    /*
     * Configurer le destinataire des frais : le premier mint_authority
     * (l'organisateur du réseau) reçoit les fees de chaque transfert.
     * Si aucun mint_authority n'est configuré, les fees sont brûlés.
     */
    if (s_currency.mint_authority_count > 0) {
        memcpy(&s_wallet.fee_recipient, &s_currency.mint_authorities[0],
               sizeof(public_key_t));
    }
    ESP_LOGI(TAG, "[8/12] Wallet initialise (fee_recipient=%s)",
             s_currency.mint_authority_count > 0 ? "mint_authority[0]" : "nul (fees brules)");

    /* Restaurer le last_melt_timestamp global depuis le checkpoint */
    if (!first_boot && s_checkpoint.last_melt_timestamp > 0) {
        s_wallet.last_melt_timestamp = s_checkpoint.last_melt_timestamp;
        ESP_LOGI(TAG, "Melt timestamp restaure depuis checkpoint");
    }

    /* ---- 9. Lock table ---- */
    ret = lock_table_init(&s_lock_table, &s_wallet);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Lock table init echoue: %d", ret);
        return;
    }
    ESP_LOGI(TAG, "[9/12] Lock table initialisee");

#if CONFIG_MESHPAY_TEST_DEVICE_SEED
    const bool legacy_initial_mint_enabled = false;
#else
    const bool legacy_initial_mint_enabled = true;
#endif

    /* ---- 10. Premier boot : crediter initial_balance via MINT ---- */
    if (legacy_initial_mint_enabled &&
        first_boot && s_currency.initial_balance > 0) {
        /* Creer un hash fictif comme parent (genese) */
        hash_t genesis_parent;
        memset(&genesis_parent, 0, sizeof(hash_t));

        transaction_t mint_tx;
        ret = tx_create_mint(&mint_tx, &s_keypair, &s_keypair.public_key,
                             s_currency.initial_balance,
                             s_currency.currency_id,
                             next_seq(),
                             &genesis_parent, 1,
                             get_tx_timestamp_wrapper());
        if (ret == ESP_OK) {
            dag_insert_and_track(&mint_tx);
            persist_runtime_checkpoint("initial_mint");
            ESP_LOGI(TAG, "[10/12] Initial balance credit: %"PRIu32,
                     s_currency.initial_balance);
        } else {
            ESP_LOGE(TAG, "[10/12] Erreur MINT initial: %d", ret);
        }
    } else {
        ESP_LOGI(TAG, "[10/12] Pas de MINT initial");
    }

#if CONFIG_MESHPAY_TEST_DEVICE_SEED
    ret = run_test_device_seed();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "[10b/12] Seed test echoue: %d", ret);
    }
#endif
    dag_integrity_log("post_init");

    /* ---- 11. HAL display + comm ---- */
    /*
     * Display : un par cible (ILI9341 sur CYD, JD9853 sur Waveshare).
     * ESP-NOW : sur les deux cibles (toute radio Wi-Fi le supporte).
     * LoRa : sur toutes les cibles ; le driver concret (Wio-E5, Core1262)
     *        est selectionne par CONFIG_MESHPAY_LORA_DRIVER.
     */
#if CONFIG_IDF_TARGET_ESP32
    hal_display_ili9341_create(&s_display);
#elif CONFIG_IDF_TARGET_ESP32S3
    hal_display_jd9853_create(&s_display);
#endif

    /*
     * Sur Waveshare ESP32-S3 + Core1262, l'init LCD a besoin d'un bus SPI
     * avec DMA interne. Si on attend la creation de ui_task, Wi-Fi/LoRa et
     * plusieurs stacks peuvent avoir deja consomme ou fragmente ce heap DMA,
     * ce qui laisse le firmware vivant mais l'ecran noir. Le driver JD9853
     * ignore les appels suivants quand il est deja initialise, donc ui_task
     * garde son chemin normal sans double-init.
     */
    hal_err_t display_err = s_display.init(s_display.ctx);
    if (display_err != HAL_OK) {
        ESP_LOGE(TAG, "Init display precoce echouee (%d)", display_err);
    }

    /*
     * [F-MN-002, F-MN-007] Création des queues et du mutex AVANT le
     * démarrage de `lora_sync_task` via `transport_lora_init_and_start`.
     *
     * Sans cet ordre, la tâche LoRa pouvait être préemptée sur l'autre
     * cœur et accéder à `s_evt_queue`/`s_state_mutex` encore à NULL
     * (assert FreeRTOS). Et on vérifie tous les handles ensemble avant
     * tout démarrage de tâche : un OOM partiel ne doit pas laisser de
     * queue orpheline avec des tâches déjà lancées.
     */
    s_evt_queue   = xQueueCreate(EVT_QUEUE_DEPTH, sizeof(comm_event_t));
    s_state_mutex = xSemaphoreCreateMutex();
    s_cmd_queue   = xQueueCreate(CMD_QUEUE_DEPTH, sizeof(comm_cmd_t));

    if (!s_evt_queue || !s_state_mutex || !s_cmd_queue) {
        ESP_LOGE(TAG, "Erreur creation queues/mutex (evt=%p state=%p cmd=%p)",
                 s_evt_queue, s_state_mutex, s_cmd_queue);
        return;
    }

    if (!start_power_and_ui_task()) {
        return;
    }

    if (espnow_hal_esp32_create(&s_espnow_hal) != HAL_OK) {
        ESP_LOGE(TAG, "ESP-NOW HAL init echoue");
        return;
    }

    /* HAL LoRa + lora_sync_task. Les handles ci-dessus sont maintenant
     * tous valides et lisibles depuis n'importe quel cœur. */
    /*
     * [F-LT-001] Capturer le retour : un echec ici signifie que LoRa
     * sera indisponible (attestation paiement skipee, pas de sync
     * periodique entre devices hors portee ESP-NOW). Le firmware
     * continue : ESP-NOW reste fonctionnel pour les peers a portee.
     */
    hal_err_t lora_err = transport_lora_init_and_start();
    if (lora_err != HAL_OK) {
        ESP_LOGW(TAG, "LoRa indisponible (%d) — fonctionnement en mode "
                      "ESP-NOW seul ; verifier cablage SX1262 / Kconfig "
                      "MESHPAY_LORA_C1262_PIN_*", lora_err);
    }

    ESP_LOGI(TAG, "[11/12] HAL initialises (ESP-NOW%s)",
             transport_lora_available() ? " + LoRa" : "");

    /* ---- 12. Tâches applicatives ---- */

    /* Configuration ESP-NOW task. ESP-NOW est present sur ESP32 + ESP32-S3
     * (les deux cibles supportees actuellement) ; pas de garde
     * conditionnelle ici. */
    static espnow_config_t espnow_cfg;
    memset(&espnow_cfg, 0, sizeof(espnow_cfg));
    espnow_cfg.hal = &s_espnow_hal;
    espnow_cfg.evt_queue = s_evt_queue;
    espnow_cfg.cmd_queue = s_cmd_queue;
    memcpy(&espnow_cfg.own_pubkey, &s_keypair.public_key, sizeof(public_key_t));
    espnow_cfg.keypair = &s_keypair;
    strncpy(espnow_cfg.own_alias, s_device_alias, sizeof(espnow_cfg.own_alias) - 1);

    BaseType_t task_ok = xTaskCreate(espnow_task, "espnow", ESPNOW_TASK_STACK,
                                     &espnow_cfg, ESPNOW_TASK_PRIO, NULL);
    if (task_ok != pdPASS) {
        ESP_LOGE(TAG, "Erreur creation tache espnow");
        return;
    }

    /* lora_sync_task est creee dans transport_lora_init_and_start (deja appele). */

    /* Tache core — commune aux deux targets */
    task_ok = xTaskCreate(core_task, "core", CORE_TASK_STACK, NULL,
                          CORE_TASK_PRIO, NULL);
    if (task_ok != pdPASS) {
        ESP_LOGE(TAG, "Erreur creation tache core");
        return;
    }

    /*
     * Console de debug serie. debug_console_dumps.c (impl reelle) ou
     * debug_console_dumps_stub.c (no-op) est selectionne par CMake selon
     * CONFIG_MESHPAY_DEBUG_CONSOLE. Appel inconditionnel.
     */
    debug_console_register_dumps();

    /* [Lot C item 8] Tache de monitoring des stacks. Priorite basse (1)
     * pour ne pas interferer avec les taches critiques. */
    task_ok = xTaskCreate(stack_monitor_task, "stkmon", STACK_MONITOR_TASK_STACK,
                          NULL, 1, NULL);
    if (task_ok != pdPASS) {
        ESP_LOGW(TAG, "Erreur creation tache stkmon");
    }

    ESP_LOGI(TAG, "[13/13] Taches lancees — systeme operationnel");

    /* app_main retourne, les taches FreeRTOS continuent */
}
