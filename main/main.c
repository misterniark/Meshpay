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
 * Etat global et capabilites (MP_HAS_ESPNOW / MP_HAS_LORA) :
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

    /* Init des callbacks de persistance checkpoint (backend NVS).
     * Pour un backend mock (test), reassigner avant l'appel au load. */
    s_checkpoint_save = nvs_checkpoint_save;
    s_checkpoint_load = nvs_checkpoint_load;

    /* ---- 4. DAG ---- */
    dag_init(&s_dag);
    ESP_LOGI(TAG, "[4/12] DAG initialise");

    /* ---- 5. Checkpoint ---- */
    bool first_boot = (s_checkpoint_load(&s_checkpoint, NULL) != ESP_OK);
    if (first_boot) {
        memset(&s_checkpoint, 0, sizeof(checkpoint_t));
        ESP_LOGI(TAG, "[5/12] Premier demarrage, pas de checkpoint");
    } else {
        ESP_LOGI(TAG, "[5/12] Checkpoint charge");
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

    /* ---- 10. Premier boot : crediter initial_balance via MINT ---- */
    if (first_boot && s_currency.initial_balance > 0) {
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
            ESP_LOGI(TAG, "[10/12] Initial balance credit: %"PRIu32,
                     s_currency.initial_balance);
        } else {
            ESP_LOGE(TAG, "[10/12] Erreur MINT initial: %d", ret);
        }
    } else {
        ESP_LOGI(TAG, "[10/12] Pas de MINT initial");
    }

    /* ---- 11. HAL display + comm ---- */
    /*
     * Display : un par cible (ILI9341 sur CYD, JD9853 sur Waveshare).
     * ESP-NOW : sur les deux cibles (toute radio Wi-Fi le supporte).
     * LoRa : sur les deux cibles — CYD et S3 Waveshare embarquent un Wio-E5.
     */
#if CONFIG_IDF_TARGET_ESP32
    hal_display_ili9341_create(&s_display);
#elif CONFIG_IDF_TARGET_ESP32S3
    hal_display_jd9853_create(&s_display);
#endif

    if (espnow_hal_esp32_create(&s_espnow_hal) != HAL_OK) {
        ESP_LOGE(TAG, "ESP-NOW HAL init echoue");
        return;
    }

    /* HAL LoRa + lora_sync_task (no-op sur cibles sans LoRa). */
    (void)transport_lora_init_and_start();

    ESP_LOGI(TAG, "[11/12] HAL initialises (ESP-NOW%s)",
             transport_lora_available() ? " + LoRa" : "");

    /* ---- 12. Queues, mutex et taches ---- */
    s_evt_queue = xQueueCreate(EVT_QUEUE_DEPTH, sizeof(comm_event_t));
    s_state_mutex = xSemaphoreCreateMutex();

    s_cmd_queue = xQueueCreate(CMD_QUEUE_DEPTH, sizeof(comm_cmd_t));
    if (!s_cmd_queue) {
        ESP_LOGE(TAG, "Erreur creation s_cmd_queue");
        return;
    }

    if (!s_evt_queue || !s_state_mutex) {
        ESP_LOGE(TAG, "Erreur creation queues/mutex");
        return;
    }

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

    xTaskCreate(espnow_task, "espnow", ESPNOW_TASK_STACK, &espnow_cfg,
                ESPNOW_TASK_PRIO, NULL);

    /* lora_sync_task est creee dans transport_lora_init_and_start (deja appele). */

    /* Tache core — commune aux deux targets */
    xTaskCreate(core_task, "core", CORE_TASK_STACK, NULL,
                CORE_TASK_PRIO, NULL);

    /*
     * Console de debug serie. debug_console_dumps.c (impl reelle) ou
     * debug_console_dumps_stub.c (no-op) est selectionne par CMake selon
     * CONFIG_MESHPAY_DEBUG_CONSOLE. Appel inconditionnel.
     */
    debug_console_register_dumps();

    /* ---- 13. UI task ---- */
    s_ui_cmd_queue = xQueueCreate(UI_CMD_QUEUE_DEPTH, sizeof(ui_cmd_t));
    if (!s_ui_cmd_queue) {
        ESP_LOGE(TAG, "Erreur creation ui_cmd_queue");
        return;
    }

    /* Remplir le contexte UI avec les pointeurs vers l'etat global */
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

    s_ui_ctx.is_master = false;
    for (uint32_t i = 0; i < s_currency.mint_authority_count; i++) {
        if (public_key_equal(&s_keypair.public_key,
                             &s_currency.mint_authorities[i])) {
            s_ui_ctx.is_master = true;
            break;
        }
    }

    xTaskCreate(ui_task, "ui", UI_TASK_STACK, &s_ui_ctx,
                UI_TASK_PRIO, NULL);

    /* [Lot C item 8] Tache de monitoring des stacks. Priorite basse (1)
     * pour ne pas interferer avec les taches critiques. */
    xTaskCreate(stack_monitor_task, "stkmon", STACK_MONITOR_TASK_STACK,
                NULL, 1, NULL);

    ESP_LOGI(TAG, "[13/13] Taches lancees — systeme operationnel");

    /* app_main retourne, les taches FreeRTOS continuent */
}
