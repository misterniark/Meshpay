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

/* Chiffrement NVS : inclus conditionnellement selon la config */
#if defined(CONFIG_NVS_ENCRYPTION)
#include "nvs_sec_provider.h"
#include "esp_partition.h"
#endif

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

/* ================================================================
 * Debug console callbacks (composant debug_console)
 *
 * Quatre callbacks de dump JSON pour le moniteur multi-device serie.
 * Chacun prend s_state_mutex avec un timeout court, itere sur l'etat
 * partage, emet une ligne JSON par item via le writer fourni, puis
 * libere le mutex. Conserves uniquement quand CONFIG_MESHPAY_DEBUG_
 * CONSOLE=y (compile-out complet en production via le header).
 *
 * Pattern identique a main_collect_confirmed_txs (Lot C item 7) :
 * inversion de dependance, le composant ne touche jamais aux types
 * applicatifs ni au mutex directement.
 * ================================================================ */

#if CONFIG_MESHPAY_DEBUG_CONSOLE

/* Taille du buffer ligne utilise par les callbacks de dump. Le
 * buffer lui-meme est alloue sur la stack a l'interieur de chaque
 * callback : la DRAM est saturee (dette technique « DRAM dram0_seg
 * saturee », ~60 octets de marge), on ne peut pas se permettre
 * 384 octets de plus en .bss. La stack de la tache dbg_console
 * (4 Ko) absorbe largement un buffer ~512 octets temporaire.
 *
 * Mono-thread garanti : tous les callbacks sont appeles en sequence
 * par la meme tache dbg_console, donc pas de souci de re-entrance.
 */
#define DBG_LINE_SIZE CONFIG_MESHPAY_DEBUG_CONSOLE_LINE_BUF_SIZE

/** Helper : encode un buffer binaire en hex via le helper du composant. */
static inline void dbg_hex(const uint8_t *src, size_t len, char *dst, size_t dst_size)
{
    debug_console_hex_encode(src, len, dst, dst_size);
}

/** Nom lisible du status TX pour le JSON. */
static const char *dbg_status_name(tx_status_t s)
{
    switch (s) {
        case TX_STATUS_LOCKED:    return "LOCKED";
        case TX_STATUS_CONFIRMED: return "CONFIRMED";
        case TX_STATUS_CANCELLED: return "CANCELLED";
        default:                  return "?";
    }
}

/**
 * @brief Dump du DAG : count, max, puis une ligne par TX.
 *
 * Format d'une ligne TX (JSON sur une seule ligne) :
 *   {"i":N,"id":"<hex>","type":"TRANSFER|MINT","from":"<hex>",
 *    "to":"<hex>","amount":N,"currency":N,"fee":N,"seq":N,
 *    "status":"LOCKED|CONFIRMED|CANCELLED","ts":N,"parents":["<hex>",...]}
 */
static void main_debug_dump_dag(debug_console_writer_fn writer, void *ctx)
{
    char line[DBG_LINE_SIZE];
    const TickType_t to = pdMS_TO_TICKS(CONFIG_MESHPAY_DEBUG_CONSOLE_MUTEX_TIMEOUT_MS);
    if (xSemaphoreTake(s_state_mutex, to) != pdTRUE) {
        writer("{\"err\":\"mutex_timeout\"}", ctx);
        return;
    }

    /* Header : compte de TX et capacite max. */
    snprintf(line, sizeof(line),
             "{\"count\":%lu,\"max\":%lu}",
             (unsigned long)s_dag.count,
             (unsigned long)DAG_MAX_TRANSACTIONS);
    writer(line, ctx);

    /* Buffers hex locaux (chaque hex = 2*32+1 = 65 octets). */
    char id_hex[CRYPTO_HASH_SIZE * 2 + 1];
    char from_hex[CRYPTO_PUBLIC_KEY_SIZE * 2 + 1];
    char to_hex[CRYPTO_PUBLIC_KEY_SIZE * 2 + 1];
    char parent_hex[CRYPTO_HASH_SIZE * 2 + 1];

    for (uint32_t i = 0; i < s_dag.count; i++) {
        const transaction_t *tx = &s_dag.transactions[i];

        dbg_hex(tx->id.bytes, sizeof(tx->id.bytes), id_hex, sizeof(id_hex));
        dbg_hex(tx->from.bytes, sizeof(tx->from.bytes), from_hex, sizeof(from_hex));
        dbg_hex(tx->to.bytes, sizeof(tx->to.bytes), to_hex, sizeof(to_hex));

        /* Construction de la ligne en deux temps :
         *   1. partie scalaire jusqu'au '[' des parents,
         *   2. enumeration des parents (1 ou 2) puis fermeture ']}'. */
        int n = snprintf(line, sizeof(line),
                         "{\"i\":%lu,\"id\":\"%s\",\"type\":\"%s\","
                         "\"from\":\"%s\",\"to\":\"%s\","
                         "\"amount\":%lu,\"currency\":%lu,\"fee\":%lu,"
                         "\"seq\":%lu,\"status\":\"%s\","
                         "\"ts\":%llu,\"parents\":[",
                         (unsigned long)i,
                         id_hex,
                         tx->type == TX_TYPE_MINT ? "MINT" : "TRANSFER",
                         from_hex, to_hex,
                         (unsigned long)tx->amount,
                         (unsigned long)tx->currency_id,
                         (unsigned long)tx->fee,
                         (unsigned long)tx->seq,
                         dbg_status_name(tx->status),
                         (unsigned long long)tx->timestamp);

        for (uint8_t p = 0; p < tx->parent_count && n > 0 && n < (int)sizeof(line); p++) {
            dbg_hex(tx->parents[p].bytes, sizeof(tx->parents[p].bytes),
                    parent_hex, sizeof(parent_hex));
            n += snprintf(line + n, sizeof(line) - n,
                          "%s\"%s\"", p > 0 ? "," : "", parent_hex);
        }
        if (n > 0 && n < (int)sizeof(line)) {
            snprintf(line + n, sizeof(line) - n, "]}");
        }
        writer(line, ctx);
    }

    xSemaphoreGive(s_state_mutex);
}

/**
 * @brief Dump du wallet : identite, alias, solde, verrous actifs.
 *
 * Une premiere ligne avec l'identite, puis une ligne par lock actif :
 *   {"own":"<hex>","alias":"...","balance":N,"fee_recipient":"<hex>",
 *    "last_melt_ts":N,"lock_count":N}
 *   {"i":N,"tx_id":"<hex>","amount":N,"lock_time":N}
 */
static void main_debug_dump_wallet(debug_console_writer_fn writer, void *ctx)
{
    char line[DBG_LINE_SIZE];
    const TickType_t to = pdMS_TO_TICKS(CONFIG_MESHPAY_DEBUG_CONSOLE_MUTEX_TIMEOUT_MS);
    if (xSemaphoreTake(s_state_mutex, to) != pdTRUE) {
        writer("{\"err\":\"mutex_timeout\"}", ctx);
        return;
    }

    /* Calcul du solde courant : wallet_get_balance_for traverse
     * checkpoint + DAG post-checkpoint. Cest la meme fonction
     * utilisee par l'UI, donc le solde affiche est exactement celui
     * que le device voit. */
    uint32_t balance = 0;
    (void)wallet_get_balance_for(&s_dag, &s_checkpoint,
                                  &s_keypair.public_key,
                                  &s_wallet.fee_recipient,
                                  &balance);

    char own_hex[CRYPTO_PUBLIC_KEY_SIZE * 2 + 1];
    char fee_hex[CRYPTO_PUBLIC_KEY_SIZE * 2 + 1];
    dbg_hex(s_keypair.public_key.bytes, sizeof(s_keypair.public_key.bytes),
            own_hex, sizeof(own_hex));
    dbg_hex(s_wallet.fee_recipient.bytes, sizeof(s_wallet.fee_recipient.bytes),
            fee_hex, sizeof(fee_hex));

    /* Compter les locks actifs. */
    uint32_t active_locks = 0;
    for (uint32_t i = 0; i < WALLET_MAX_LOCKS; i++) {
        if (s_lock_table.entries[i].active) active_locks++;
    }

    snprintf(line, sizeof(line),
             "{\"own\":\"%s\",\"alias\":\"%.*s\","
             "\"balance\":%lu,\"fee_recipient\":\"%s\","
             "\"last_melt_ts\":%llu,\"lock_count\":%lu,"
             "\"max_locks\":%lu}",
             own_hex,
             (int)s_device_alias_len, s_device_alias,
             (unsigned long)balance,
             fee_hex,
             (unsigned long long)s_wallet.last_melt_timestamp,
             (unsigned long)active_locks,
             (unsigned long)WALLET_MAX_LOCKS);
    writer(line, ctx);

    /* Une ligne par lock actif. */
    char lock_hex[CRYPTO_HASH_SIZE * 2 + 1];
    for (uint32_t i = 0; i < WALLET_MAX_LOCKS; i++) {
        const lock_entry_t *lk = &s_lock_table.entries[i];
        if (!lk->active) continue;
        dbg_hex(lk->tx_id.bytes, sizeof(lk->tx_id.bytes),
                lock_hex, sizeof(lock_hex));
        snprintf(line, sizeof(line),
                 "{\"i\":%lu,\"tx_id\":\"%s\","
                 "\"amount\":%lu,\"lock_time\":%llu}",
                 (unsigned long)i, lock_hex,
                 (unsigned long)lk->amount,
                 (unsigned long long)lk->lock_time);
        writer(line, ctx);
    }

    xSemaphoreGive(s_state_mutex);
}

/**
 * @brief Dump de la config monnaie : nom, symbole, parametres, autorites.
 *
 *   {"id":N,"name":"...","symbol":"...","decimals":N,
 *    "max_supply":N,"valid_until":N,"initial_balance":N,
 *    "transfer_fee":N,"melt_enabled":bool,"melt_period":N,
 *    "melt_mode":"BPS|FIXED","melt_bps":N,"melt_fixed":N,
 *    "mint_authority_count":N}
 *   {"i":N,"pubkey":"<hex>"}   (une ligne par mint_authority)
 */
static void main_debug_dump_currency(debug_console_writer_fn writer, void *ctx)
{
    char line[DBG_LINE_SIZE];
    const TickType_t to = pdMS_TO_TICKS(CONFIG_MESHPAY_DEBUG_CONSOLE_MUTEX_TIMEOUT_MS);
    if (xSemaphoreTake(s_state_mutex, to) != pdTRUE) {
        writer("{\"err\":\"mutex_timeout\"}", ctx);
        return;
    }

    snprintf(line, sizeof(line),
             "{\"id\":%lu,\"name\":\"%s\",\"symbol\":\"%s\","
             "\"decimals\":%u,\"max_supply\":%llu,"
             "\"valid_until\":%llu,\"initial_balance\":%lu,"
             "\"transfer_fee\":%lu,"
             "\"melt_enabled\":%s,\"melt_period\":%lu,"
             "\"melt_mode\":\"%s\",\"melt_bps\":%u,"
             "\"melt_fixed\":%lu,\"mint_authority_count\":%u}",
             (unsigned long)s_currency.currency_id,
             s_currency.name, s_currency.symbol,
             (unsigned)s_currency.decimals,
             (unsigned long long)s_currency.max_supply,
             (unsigned long long)s_currency.valid_until,
             (unsigned long)s_currency.initial_balance,
             (unsigned long)s_currency.transfer_fee,
             s_currency.melt_enabled ? "true" : "false",
             (unsigned long)s_currency.melt_period_seconds,
             s_currency.melt_volume_mode == MELT_MODE_BPS ? "BPS" : "FIXED",
             (unsigned)s_currency.melt_bps,
             (unsigned long)s_currency.melt_fixed_amount,
             (unsigned)s_currency.mint_authority_count);
    writer(line, ctx);

    char auth_hex[CRYPTO_PUBLIC_KEY_SIZE * 2 + 1];
    for (uint8_t i = 0; i < s_currency.mint_authority_count; i++) {
        dbg_hex(s_currency.mint_authorities[i].bytes,
                sizeof(s_currency.mint_authorities[i].bytes),
                auth_hex, sizeof(auth_hex));
        snprintf(line, sizeof(line),
                 "{\"i\":%u,\"pubkey\":\"%s\"}", (unsigned)i, auth_hex);
        writer(line, ctx);
    }

    xSemaphoreGive(s_state_mutex);
}

/**
 * @brief Dump du time_manager : mode, Lamport, etat maitre.
 *
 *   {"mode":"LAMPORT|MASTER","lamport":N,"master_valid":bool,
 *    "master_offset_ms":N,"last_master_update":N,
 *    "master_key":"<hex>"}
 */
static void main_debug_dump_time(debug_console_writer_fn writer, void *ctx)
{
    char line[DBG_LINE_SIZE];
    const TickType_t to = pdMS_TO_TICKS(CONFIG_MESHPAY_DEBUG_CONSOLE_MUTEX_TIMEOUT_MS);
    if (xSemaphoreTake(s_state_mutex, to) != pdTRUE) {
        writer("{\"err\":\"mutex_timeout\"}", ctx);
        return;
    }

    char master_hex[CRYPTO_PUBLIC_KEY_SIZE * 2 + 1];
    dbg_hex(s_time_manager.current_master_key.bytes,
            sizeof(s_time_manager.current_master_key.bytes),
            master_hex, sizeof(master_hex));

    snprintf(line, sizeof(line),
             "{\"mode\":\"%s\",\"lamport\":%llu,"
             "\"master_valid\":%s,\"master_offset_ms\":%lld,"
             "\"last_master_update\":%llu,\"master_key\":\"%s\"}",
             s_time_manager.mode == TIME_MODE_MASTER ? "MASTER" : "LAMPORT",
             (unsigned long long)time_manager_get_lamport(&s_time_manager),
             s_time_manager.master_valid ? "true" : "false",
             (long long)s_time_manager.master_offset_ms,
             (unsigned long long)s_time_manager.last_master_update,
             master_hex);
    writer(line, ctx);

    xSemaphoreGive(s_state_mutex);
}

#endif /* CONFIG_MESHPAY_DEBUG_CONSOLE */

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

/* ================================================================
 * Calcul de solde coherent checkpoint + DAG
 * ================================================================ */

/**
 * @brief Calcule le solde du owner du wallet (checkpoint + DAG courant).
 *
 * Avant l'introduction de dag_prune_before, le DAG contenait toutes les
 * transactions historiques et passer base_balance=0 suffisait. Maintenant
 * que le DAG est elague apres chaque checkpoint, les TX consolidees
 * disparaissent du DAG — on doit donc lire leur effet depuis le
 * checkpoint comme point de depart.
 *
 * Logique : base = checkpoint.balance[owner] (0 si absent)
 *          solde = base + delta(DAG_post_checkpoint)
 *
 * Doit etre appelee sous s_state_mutex (acces s_checkpoint + s_dag).
 *
 * @param[out] out_balance Solde calcule
 * @return ESP_OK en cas de succes
 */
esp_err_t compute_owner_balance(uint32_t *out_balance)
{
    if (out_balance == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /*
     * Recuperer le solde consolide dans le checkpoint.
     * checkpoint_get_balance retourne ESP_ERR_NOT_FOUND si le owner
     * n'est pas dans le checkpoint — dans ce cas base=0 (pas d'erreur).
     */
    uint32_t base = 0;
    (void)checkpoint_get_balance(&s_checkpoint, &s_keypair.public_key, &base);

    /*
     * Appliquer les TX du DAG (qui ne contient que les TX post-checkpoint
     * grace a dag_prune_before). Le parcours wallet_get_balance ajoutera
     * les credits/debits du owner depuis la base.
     */
    return wallet_get_balance(&s_wallet, base, out_balance);
}

/**
 * @brief Version exposee a l'UI via callback — retourne le solde brut.
 *
 * Utilisee par le ctx UI. Ne prend pas le mutex (appelee par l'UI
 * qui le detient deja au moment du rafraichissement d'ecran).
 */
uint32_t ui_get_owner_balance(void)
{
    uint32_t balance = 0;
    compute_owner_balance(&balance);
    return balance;
}

/* ================================================================
 * Insertion DAG avec checkpoint automatique
 * ================================================================ */

/**
 * @brief Declenche un checkpoint automatique si le DAG est suffisamment rempli.
 *
 * Utilise dag_needs_checkpoint() (seuil a 80% de la capacite) au lieu
 * d'un compteur d'insertions. Apres le checkpoint, le DAG est elague
 * via dag_prune_before() pour liberer de la place.
 *
 * Doit etre appelee sous le mutex s_state_mutex.
 */
void auto_checkpoint_if_needed(void)
{
    if (!dag_needs_checkpoint(&s_dag)) {
        return;
    }

    checkpoint_t new_chk;
    esp_err_t ret = checkpoint_create(&s_dag, &s_checkpoint,
                                      &s_currency.mint_authorities[0], &new_chk);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Echec creation checkpoint automatique (%d)", ret);
        return;
    }

    /*
     * Appliquer la fonte globale a tous les comptes du checkpoint.
     * Un seul timestamp global suffit (meme periode pour tous).
     * La fonte est un ajustement local (pas de TX dans le DAG).
     * Tous les devices appliquent la meme formule, donc convergent.
     */
    if (s_currency.melt_enabled &&
        s_time_manager.mode == TIME_MODE_MASTER &&
        time_manager_has_valid_master(&s_time_manager)) {
        uint64_t now = get_time_ms_wrapper();
        uint64_t last_ts = s_checkpoint.last_melt_timestamp;
        uint32_t ticks = currency_melt_ticks_due(
            &s_currency, last_ts, now);
        if (ticks > 0) {
            for (uint32_t i = 0; i < new_chk.account_count; i++) {
                new_chk.accounts[i].balance =
                    currency_melt_apply(&s_currency,
                                       new_chk.accounts[i].balance,
                                       ticks);
            }
            new_chk.last_melt_timestamp =
                currency_melt_next_timestamp(
                    &s_currency, last_ts, ticks, now);
            ESP_LOGI(TAG, "Fonte checkpoint: %"PRIu32" ticks",
                     ticks);
        } else {
            new_chk.last_melt_timestamp = last_ts;
        }
    }

    /* Adopter le nouveau checkpoint */
    memcpy(&s_checkpoint, &new_chk, sizeof(checkpoint_t));
    s_checkpoint_save(&s_checkpoint, NULL);

    /*
     * Elaguer le DAG : supprimer les transactions consolidees dans le
     * checkpoint (timestamp <= checkpoint.timestamp). Sans cet elagage,
     * le DAG se remplit jusqu'a DAG_MAX_TRANSACTIONS et bloque toute
     * nouvelle insertion.
     */
    dag_prune_before(&s_dag, new_chk.timestamp);

    ESP_LOGI(TAG, "Checkpoint automatique cree + DAG elague "
             "(reste %"PRIu32" TX)", s_dag.count);
}

/**
 * @brief Insere une TX dans le DAG et declenche un checkpoint si necessaire.
 *
 * Doit etre appelee sous le mutex s_state_mutex.
 */
esp_err_t dag_insert_and_track(const transaction_t *tx)
{
    esp_err_t ret = dag_insert(&s_dag, tx);
    if (ret != ESP_OK) {
        return ret;
    }

    auto_checkpoint_if_needed();
    return ESP_OK;
}

/* Handlers d'evenements deplaces dans main/handlers/ au Lot D.4 :
 *   - handler_payment.c    : peer/tx/ack/timeout/attestation
 *   - handler_time_sync.c  : time_sync
 *   - handler_broadcast.c  : broadcast + cache anti-boucle
 *   - handler_ping_pong.c  : ping/pong + cache anti-boucle
 *   - handler_admin.c      : set_alias / set_beneficiary
 */
#include "handlers/handlers.h"
/* Auto-forward, paiement, mint et ops maitre deplaces dans main/ops/
 * au Lot D.5 :
 *   - op_payment.c             : initiate_payment
 *   - op_mint.c                : initiate_mint
 *   - op_beneficiary_forward.c : attempt_beneficiary_forward
 *   - op_master.c              : broadcast_text_send, ping_send,
 *                                set_alias_send, set_beneficiary_send
 */
#include "ops/ops.h"

/**
 * @brief Verifie les verrous expires et annule les TX correspondantes.
 *
 * Appelee periodiquement dans la boucle core_task. Restera ici jusqu'au
 * Lot D.6 (core_task migration).
 */
static void check_lock_expirations(void)
{
    hash_t expired_ids[WALLET_MAX_LOCKS];
    uint32_t expired_count = 0;
    lock_table_expire(&s_lock_table, expired_ids, WALLET_MAX_LOCKS, &expired_count);
    for (uint32_t i = 0; i < expired_count; i++) {
        dag_set_status(&s_dag, &expired_ids[i], TX_STATUS_CANCELLED);
        ESP_LOGW(TAG, "Verrou expire, TX annulee");
    }
}

/* ================================================================
 * Tache principale (core_task)
 * ================================================================ */

/**
 * @brief Boucle d'evenements centrale.
 *
 * Attend les evenements sur evt_queue avec un timeout de 1s.
 * En cas de timeout, verifie les expirations de verrous.
 * Chaque evenement est traite sous le mutex s_state_mutex.
 */
/**
 * Traite une commande envoyee par l'UI.
 *
 * Appelee sous le mutex dans core_task. Delegue l'action reelle
 * (paiement, MINT, broadcast, etc.) aux fonctions existantes.
 */
static void handle_ui_command(const ui_cmd_t *cmd)
{
    switch (cmd->type) {
        case UI_CMD_PAY: {
            ESP_LOGI(TAG, "UI CMD: Paiement %"PRIu32" vers peer",
                     cmd->data.pay.amount);
            esp_err_t pay_ret = initiate_payment(&cmd->data.pay.to,
                                                  cmd->data.pay.amount);
            /* Notifier l'UI du resultat du paiement */
            if (pay_ret == ESP_OK) {
                s_pay_feedback = UI_PAY_FEEDBACK_OK;
            } else if (pay_ret == ESP_ERR_INVALID_STATE) {
                s_pay_feedback = UI_PAY_FEEDBACK_NO_FUNDS;
            } else {
                s_pay_feedback = UI_PAY_FEEDBACK_FAIL;
                ESP_LOGW(TAG, "Paiement echoue: %s", esp_err_to_name(pay_ret));
            }
            break;
        }

        case UI_CMD_MINT: {
            ESP_LOGI(TAG, "UI CMD: MINT %"PRIu32" credits",
                     cmd->data.mint.amount);
            esp_err_t mint_ret = initiate_mint(&cmd->data.mint.to,
                                                cmd->data.mint.amount);
            if (mint_ret != ESP_OK) {
                ESP_LOGW(TAG, "MINT echoue: %s", esp_err_to_name(mint_ret));
            }
            break;
        }

        case UI_CMD_DISCOVER_PEERS:
            ESP_LOGI(TAG, "UI CMD: Discover peers");
            {
                comm_cmd_t disc_cmd;
                memset(&disc_cmd, 0, sizeof(disc_cmd));
                disc_cmd.type = COMM_CMD_START_DISCOVER;
                xQueueSend(s_cmd_queue, &disc_cmd, pdMS_TO_TICKS(100));
            }
            break;

        case UI_CMD_BROADCAST_TEXT:
            ESP_LOGI(TAG, "UI CMD: Broadcast texte (%u chars)",
                     cmd->data.broadcast.text_len);
            /* op_master compile partout : runtime check `is_master` +
             * transport_lora no-op sur cibles sans LoRa. */
            broadcast_text_send(cmd->data.broadcast.text,
                                cmd->data.broadcast.text_len);
            break;

        case UI_CMD_PING:
            ESP_LOGI(TAG, "UI CMD: Ping");
            ping_send();
            break;

        case UI_CMD_SET_ALIAS:
            ESP_LOGI(TAG, "UI CMD: Set alias");
            set_alias_send(&cmd->data.set_alias.target,
                           cmd->data.set_alias.alias,
                           cmd->data.set_alias.alias_len);
            break;

        case UI_CMD_SET_BENEFICIARY:
            ESP_LOGI(TAG, "UI CMD: Set beneficiary");
            set_beneficiary_send(&cmd->data.set_beneficiary.target,
                                 &cmd->data.set_beneficiary.beneficiary,
                                 cmd->data.set_beneficiary.interval_min);
            break;

        default:
            ESP_LOGW(TAG, "UI CMD inconnue: %d", cmd->type);
            break;
    }
}

static void core_task(void *param)
{
    (void)param;
    ESP_LOGI(TAG, "core_task demarre");

    comm_event_t evt;

    for (;;) {
        /* Attendre un evenement avec timeout 1s */
        BaseType_t got = xQueueReceive(s_evt_queue, &evt, pdMS_TO_TICKS(1000));

        /* Prendre le mutex pour acceder a l'etat partage */
        if (xSemaphoreTake(s_state_mutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
            ESP_LOGE(TAG, "core_task: impossible de prendre le mutex");
            continue;
        }

        if (got == pdTRUE) {
            /* Traiter l'evenement */
            switch (evt.type) {
                case COMM_EVT_PEER_DISCOVERED:
                    handle_peer_discovered(&evt);
                    break;

                case COMM_EVT_TX_RECEIVED:
                case COMM_EVT_LORA_TX_RECEIVED:
                    handle_tx_received(&evt);
                    break;

                case COMM_EVT_ACK_RECEIVED:
                    handle_ack_received(&evt);
                    break;

                case COMM_EVT_TX_TIMEOUT:
                    handle_tx_timeout(&evt);
                    break;

                case COMM_EVT_TIME_SYNC_RECEIVED:
                    handle_time_sync(&evt);
                    break;

                case COMM_EVT_BROADCAST_RECEIVED:
                    handle_broadcast_received(&evt);
                    break;

                case COMM_EVT_PING_RECEIVED:
                    handle_ping_received(&evt);
                    break;

                case COMM_EVT_PONG_RECEIVED:
                    handle_pong_received(&evt);
                    break;

                case COMM_EVT_SET_ALIAS_RECEIVED:
                    handle_set_alias_received(&evt);
                    break;

                case COMM_EVT_SET_BENEFICIARY_RECEIVED:
                    handle_set_beneficiary_received(&evt);
                    break;

                case COMM_EVT_ATTESTATION_RECEIVED:
                    handle_attestation_received(&evt);
                    break;

                default:
                    ESP_LOGW(TAG, "Evenement inconnu: %d", evt.type);
                    break;
            }
        }

        /* Traiter les commandes UI (non-bloquant) */
        {
            ui_cmd_t ui_cmd;
            while (xQueueReceive(s_ui_cmd_queue, &ui_cmd,
                                  0) == pdTRUE) {
                handle_ui_command(&ui_cmd);
            }
        }

        /* Verification periodique des expirations de verrous */
        uint64_t now = get_time_ms_wrapper();
        if (now - s_last_expire_check >= LOCK_EXPIRE_INTERVAL_MS) {
            check_lock_expirations();
            s_last_expire_check = now;
        }

        /* Forward periodique vers le beneficiaire (si actif) */
        if (s_forward_interval_min > 0) {
            uint64_t interval_ms = (uint64_t)s_forward_interval_min * 60000;
            if (now - s_last_forward_ms >= interval_ms) {
                attempt_beneficiary_forward();
                s_last_forward_ms = now;
            }
        }

        /* Application periodique de la fonte (si activee).
         * Met a jour le last_melt_timestamp meme sans transaction,
         * evitant un rattrapage massif de ticks apres une longue inactivite. */
        if (s_currency.melt_enabled) {
            uint32_t melt_balance = 0;
            compute_owner_balance(&melt_balance);
            apply_pending_melt(&melt_balance);
        }

        xSemaphoreGive(s_state_mutex);

        /*
         * Pomper les envois LoRa differes (relay broadcast/ping +
         * PONG signe). Sur stub : no-op. Sur reel : applique les delais
         * aleatoires anti-collision et appelle s_lora_hal.send.
         */
        transport_lora_pump();
    }
}

/* ================================================================
 * Point d'entree
 * ================================================================ */

void app_main(void)
{
    ESP_LOGI(TAG, "=== Offline Payment System - Demarrage ===");

    /* ---- 1. NVS (avec chiffrement si CONFIG_NVS_ENCRYPTION) ---- */
    esp_err_t ret;

#if defined(CONFIG_NVS_ENCRYPTION)
    /*
     * Initialisation NVS securisee :
     * 1. Trouver la partition nvs_keys (contient les cles de chiffrement)
     * 2. Obtenir la configuration de chiffrement via le provider
     * 3. Initialiser NVS avec chiffrement AES-XTS
     *
     * Si la partition nvs_keys est vierge (premier boot), les cles sont
     * generees automatiquement et stockees dans la partition (elle-meme
     * protegee par le chiffrement flash materiel).
     */
    ESP_LOGI(TAG, "NVS: initialisation avec chiffrement active");

    /* Trouver la partition nvs_keys */
    const esp_partition_t *keys_part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_NVS_KEYS, NULL);

    if (keys_part == NULL) {
        ESP_LOGE(TAG, "Partition nvs_keys introuvable — chiffrement NVS impossible");
        ESP_LOGE(TAG, "Verifiez que partitions.csv contient une partition nvs_keys");
        return;
    }

    /* Obtenir la config de chiffrement via le provider flash-encryption */
    nvs_sec_cfg_t nvs_sec_cfg;
    nvs_sec_scheme_t *sec_scheme = NULL;
    nvs_sec_config_flash_enc_t fe_cfg = NVS_SEC_PROVIDER_CFG_FLASH_ENC_DEFAULT();
    ret = nvs_sec_provider_register_flash_enc(&fe_cfg, &sec_scheme);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Echec enregistrement du scheme NVS sec: 0x%x", ret);
        return;
    }

    ret = nvs_flash_read_security_cfg_v2(sec_scheme, &nvs_sec_cfg);
    if (ret == ESP_ERR_NVS_KEYS_NOT_INITIALIZED) {
        /* Premier boot : generer les cles de chiffrement NVS */
        ESP_LOGW(TAG, "NVS: premier boot, generation des cles de chiffrement");
        ret = nvs_flash_generate_keys_v2(sec_scheme, &nvs_sec_cfg);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Echec generation cles NVS: 0x%x", ret);
            return;
        }
    } else if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Echec lecture config securite NVS: 0x%x", ret);
        return;
    }

    /* Initialisation NVS securisee */
    ret = nvs_flash_secure_init(&nvs_sec_cfg);
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS: effacement et reinitialisation securisee");
        nvs_flash_erase();
        ret = nvs_flash_secure_init(&nvs_sec_cfg);
#else
    /* Initialisation NVS standard (sans chiffrement) */
    ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS: effacement et reinitialisation");
        nvs_flash_erase();
        ret = nvs_flash_init();
#endif /* CONFIG_NVS_ENCRYPTION */

        /* Securite [C11] : apres effacement NVS, initialiser le compteur
         * d'echecs PIN a la valeur de verrouillage maximale. Cela empeche
         * un attaquant de contourner le brute-force en effacant le NVS
         * (qui remettrait normalement le compteur a zero). Le device
         * sera verrouille et necessitera un factory reset intentionnel. */
        if (ret == ESP_OK) {
            nvs_handle_t h;
            if (nvs_open("pin", NVS_READWRITE, &h) == ESP_OK) {
                uint32_t max_fails = 10;  /* Valeur de verrouillage */
                nvs_set_u32(h, "pin_fails", max_fails);
                nvs_commit(h);
                nvs_close(h);
                ESP_LOGW(TAG, "NVS efface : compteur PIN verrouille (anti brute-force)");
            }
        }
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "NVS init echoue: 0x%x", ret);
        return;
    }
#if defined(CONFIG_NVS_ENCRYPTION)
    ESP_LOGI(TAG, "[1/12] NVS initialise (chiffre)");
#else
    ESP_LOGI(TAG, "[1/12] NVS initialise");
#endif

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
     * LoRa : uniquement sur les cartes qui embarquent un Wio-E5.
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

    /* ---- Console de debug serie (composant debug_console) ----
     * Active quand CONFIG_MESHPAY_DEBUG_CONSOLE=y (= mode prototype,
     * voir Kconfig du composant pour la regle de default basee sur
     * SECURE_FLASH_ENCRYPTION_MODE_RELEASE). Quand desactivee, l'appel
     * se replie sur un stub static inline (zero code embarque). */
#if CONFIG_MESHPAY_DEBUG_CONSOLE
    static const debug_console_callbacks_t s_debug_cbs = {
        .dump_dag      = main_debug_dump_dag,
        .dump_wallet   = main_debug_dump_wallet,
        .dump_currency = main_debug_dump_currency,
        .dump_time     = main_debug_dump_time,
    };
    esp_err_t dbg_err = debug_console_init(&s_debug_cbs);
    if (dbg_err != ESP_OK) {
        ESP_LOGW(TAG, "Console de debug : init echouee (err=0x%x)",
                 (int)dbg_err);
    } else {
        ESP_LOGI(TAG, "Console de debug active (commandes : help, "
                      "dump_dag, dump_wallet, dump_currency, "
                      "dump_time, dump_all).");
    }
#endif

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
