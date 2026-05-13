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

#ifdef MP_HAS_ESPNOW
extern hal_err_t espnow_hal_esp32_create(espnow_hal_t *hal);
#endif
#ifdef MP_HAS_LORA
extern hal_err_t hal_lora_wio_e5_create(hal_lora_t *lora,
                                        int uart_num, int tx_pin, int rx_pin);
#endif
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
static esp_err_t compute_owner_balance(uint32_t *out_balance)
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
static uint32_t ui_get_owner_balance(void)
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
static void auto_checkpoint_if_needed(void)
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
static esp_err_t dag_insert_and_track(const transaction_t *tx)
{
    esp_err_t ret = dag_insert(&s_dag, tx);
    if (ret != ESP_OK) {
        return ret;
    }

    auto_checkpoint_if_needed();
    return ESP_OK;
}

/* ================================================================
 * Handlers d'evenements
 * ================================================================ */

/**
 * @brief Traite la decouverte d'un nouveau peer.
 */
static void handle_peer_discovered(const comm_event_t *evt)
{
    add_peer(&evt->data.peer);
}

/**
 * @brief Traite la reception d'une transaction (via ESP-NOW ou LoRa).
 *
 * Pipeline de validation :
 * 1. Valider les regles currency (pas dans dag_merge)
 * 2. Fusionner via dag_merge_transaction (doublon, structure, signature, MINT)
 * 3. Si INSERTED : checkpoint automatique si DAG rempli
 * 4. Si TRANSFER vers nous : creer et envoyer un ACK
 * 5. Mettre a jour le compteur Lamport
 */
static void handle_tx_received(const comm_event_t *evt)
{
    const transaction_t *rx_tx = &evt->data.tx;

    /*
     * 1. Validation currency (regles metier : plafond, expiration, montants).
     * dag_merge_transaction ne fait pas ce check — il ne connait que la
     * cryptographie (structure, signature, master keys).
     */
    /*
     * [C2-fix] Calculer le solde du VRAI emetteur (rx_tx->from) a partir
     * de notre etat local (checkpoint + DAG post-checkpoint).
     *
     * LIMITE : on ne connait le solde du `from` qu'a partir des TX qu'on
     * a observees. Si le `from` a emis des TX qu'on n'a pas encore recues,
     * on sous-estime sa depense → on peut accepter a tort une TX en
     * double-depense. Cette validation est une defense en profondeur,
     * la vraie garantie anti-double-depense reste :
     *   - lock source cote emetteur (wallet_lock) ;
     *   - detection de conflits via nonce monotone (I3).
     */
    uint32_t sender_balance = 0;
    const public_key_t *fee_recipient =
        (s_currency.mint_authority_count > 0)
            ? &s_currency.mint_authorities[0] : NULL;
    wallet_get_balance_for(&s_dag, &s_checkpoint, &rx_tx->from,
                           fee_recipient, &sender_balance);

    uint64_t total_minted = 0;
    wallet_get_total_minted(&s_dag, &total_minted);

    currency_err_t cerr = currency_validate(&s_currency, rx_tx,
                                            get_time_ms_wrapper(),
                                            sender_balance, total_minted, 0);
    if (cerr != CURRENCY_OK) {
        ESP_LOGW(TAG, "TX recue : regle currency violee (%d)", cerr);
        return;
    }

    /*
     * 2. Fusion dans le DAG via dag_merge_transaction.
     * Gere : doublons (DUPLICATE), DAG plein (REJECTED),
     * validation structure, signature Ed25519, et MINT master keys.
     */
    master_keys_t mk = {
        .keys  = s_currency.mint_authorities,
        .count = s_currency.mint_authority_count,
    };

    dag_merge_result_t merge_result;
    esp_err_t ret = dag_merge_transaction(&s_dag, rx_tx, &mk, &merge_result);

    if (merge_result == DAG_MERGE_DUPLICATE) {
        /* Transaction deja connue — rien a faire */
        return;
    }
    if (merge_result != DAG_MERGE_INSERTED) {
        ESP_LOGW(TAG, "TX recue : merge rejete (result=%d, err=%d)",
                 merge_result, ret);
        return;
    }

    /* 3. Checkpoint automatique si le DAG atteint le seuil (80%) */
    auto_checkpoint_if_needed();

    ESP_LOGI(TAG, "TX recue et inseree (amount=%"PRIu32")", rx_tx->amount);

    /* 5. Si c'est un TRANSFER vers nous, envoyer un ACK */
    if (rx_tx->type == TX_TYPE_TRANSFER &&
        public_key_equal(&rx_tx->to, &s_keypair.public_key)) {

        /* Confirmer la TX dans le DAG */
        dag_set_status(&s_dag, &rx_tx->id, TX_STATUS_CONFIRMED);

#ifdef MP_HAS_ESPNOW
        /* Envoyer ACK via ESP-NOW (courte portée, direct à l'émetteur) */
        const uint8_t *dest_mac = find_peer_mac(&rx_tx->from);
        if (dest_mac != NULL) {
            comm_cmd_t cmd;
            memset(&cmd, 0, sizeof(cmd));
            cmd.type = COMM_CMD_SEND_ACK;
            memcpy(&cmd.data.send_ack.tx_id, &rx_tx->id, sizeof(hash_t));
            memcpy(cmd.data.send_ack.dest_mac, dest_mac, 6);
            xQueueSend(s_cmd_queue, &cmd, 0);
        }
#endif

#ifdef MP_HAS_LORA
        /*
         * [I2-fix] Diffuser aussi une ATTESTATION signée en LoRa.
         *
         * L'ACK ESP-NOW ne couvre que les pairs à courte portée (~200m).
         * L'attestation LoRa (~2km) permet au reste du réseau de savoir
         * que cette TX est confirmée — sans cela, les pairs qui reçoivent
         * la TX en sync LoRa la gardent en LOCKED pour toujours (le champ
         * status est forcé à LOCKED en réception par durcissement).
         *
         * La signature Ed25519 couvre tx_id (32 octets). Les récepteurs
         * vérifient signature + que attester_key == tx.to avant de
         * promouvoir la TX à CONFIRMED localement.
         *
         * NB : l'ESP32-S3 étant actuellement client-only (pas de LoRa),
         * il ne peut pas émettre d'attestation. Les devices ESP32 dans
         * son environnement le feront quand ils recevront la TX via LoRa.
         * Une évolution future (LoRa sur ESP32-S3) permettra l'autonomie.
         */
        signature_t att_sig;
        if (crypto_sign(rx_tx->id.bytes, CRYPTO_HASH_SIZE,
                        &s_keypair, &att_sig) == ESP_OK) {
            uint8_t att_buf[COMM_MSG_ATTESTATION_SIZE];
            size_t att_len = 0;
            if (comm_msg_pack_attestation(att_buf, sizeof(att_buf),
                                          &s_keypair.public_key,
                                          &att_sig, &rx_tx->id,
                                          &att_len) == 0) {
                hal_err_t herr = s_lora_hal.send(att_buf, att_len, s_lora_hal.ctx);
                if (herr != HAL_OK) {
                    ESP_LOGW(TAG, "Echec envoi attestation LoRa: %d", herr);
                }
            }
        } else {
            ESP_LOGE(TAG, "Echec signature attestation");
        }
#endif

        ESP_LOGI(TAG, "TX confirmée + ACK/attestation diffusés (amount=%"PRIu32")",
                 rx_tx->amount);
    }

    /* 6. Mettre a jour Lamport */
    time_manager_on_tx_received(&s_time_manager, rx_tx->timestamp);
}

/**
 * @brief Traite la reception d'un ACK.
 *
 * Confirme le verrou et passe la TX a CONFIRMED dans le DAG.
 */
static void handle_ack_received(const comm_event_t *evt)
{
    const hash_t       *tx_id      = &evt->data.ack.tx_id;
    const public_key_t *sender_key = &evt->data.ack.sender_key;

    /*
     * [C4-fix] Verifier que le signataire de l'ACK est bien le destinataire
     * attendu de la transaction verrouillee.
     *
     * Avant ce fix, n'importe quel peer observant un tx_id en circulation
     * pouvait signer un ACK avec sa propre cle et faire confirmer
     * localement une transaction que le vrai destinataire n'avait jamais
     * recue — faille permettant un faux "accuse de reception".
     *
     * Le flow correct :
     * 1. Retrouver la TX locale par tx_id (celle qu'on a emise et verrouillee)
     * 2. Comparer ack.sender_key avec tx.to
     * 3. Si different → ACK forge, rejeter
     * 4. Sinon → confirmer normalement
     *
     * La signature Ed25519 de l'ACK a deja ete verifiee par espnow.c
     * (ligne 284) avec cette meme sender_key. On verifie ici l'IDENTITE
     * de ce signataire, pas la validite cryptographique.
     */
    const transaction_t *tx = dag_get_by_id(&s_dag, tx_id);
    if (tx == NULL) {
        ESP_LOGW(TAG, "ACK recu pour TX inconnue dans le DAG");
        return;
    }

    if (!public_key_equal(&tx->to, sender_key)) {
        ESP_LOGW(TAG, "ACK rejete : sender_key != tx.to (forge d'ACK ?)");
        return;
    }

    /* Confirmer le verrou (libere le slot, le montant reste depense) */
    esp_err_t ret = lock_table_confirm(&s_lock_table, tx_id);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "ACK recu pour verrou inconnu");
        return;
    }

    /* Passer la TX a CONFIRMED dans le DAG */
    dag_set_status(&s_dag, tx_id, TX_STATUS_CONFIRMED);

    ESP_LOGI(TAG, "TX confirmee (ACK recu et verifie)");
}

/**
 * @brief Traite un timeout de transaction.
 *
 * Annule le verrou et passe la TX a CANCELLED dans le DAG.
 */
static void handle_tx_timeout(const comm_event_t *evt)
{
    const hash_t *tx_id = &evt->data.tx_id;

    lock_table_cancel(&s_lock_table, tx_id);
    dag_set_status(&s_dag, tx_id, TX_STATUS_CANCELLED);

    ESP_LOGW(TAG, "TX annulee (timeout)");
}

/**
 * @brief [I2-fix] Traite une attestation de confirmation signée reçue en LoRa.
 *
 * Ces attestations permettent à un pair hors portée ESP-NOW de savoir
 * qu'une TX verrouillée a été confirmée par son destinataire. Sans cela,
 * les TX LoRa restent à jamais LOCKED côté récepteur (durcissement
 * anti-usurpation du status — voir lora_sync handle_rx).
 *
 * Vérifications :
 *  1. La signature a déjà été vérifiée par lora_sync (hors mutex).
 *  2. La TX existe dans le DAG local.
 *  3. attester_key == tx.to (seul le destinataire peut attester).
 *  4. Si OK → dag_set_status(CONFIRMED).
 *
 * Idempotent : si la TX est déjà CONFIRMED, on ne fait rien.
 */
static void handle_attestation_received(const comm_event_t *evt)
{
    const comm_msg_attestation_t *att = &evt->data.attestation;

    const transaction_t *tx = dag_get_by_id(&s_dag, &att->tx_id);
    if (tx == NULL) {
        ESP_LOGD(TAG, "Attestation reçue pour TX inconnue — ignorée");
        return;
    }

    if (tx->status == TX_STATUS_CONFIRMED) {
        /* Déjà confirmée, rien à faire */
        return;
    }

    if (tx->status == TX_STATUS_CANCELLED) {
        ESP_LOGW(TAG, "Attestation reçue pour TX annulée — ignorée");
        return;
    }

    /* Vérifier que l'attester est bien le destinataire légitime de la TX */
    if (!public_key_equal(&att->attester_key, &tx->to)) {
        ESP_LOGW(TAG, "Attestation rejetée : attester_key != tx.to "
                 "(tentative d'attestation par un tiers)");
        return;
    }

    /* Confirmer la TX dans le DAG */
    dag_set_status(&s_dag, &att->tx_id, TX_STATUS_CONFIRMED);
    ESP_LOGI(TAG, "TX confirmée par attestation LoRa (amount=%"PRIu32")",
             tx->amount);
}

/**
 * @brief Traite la reception d'un message TIME_SYNC via LoRa.
 */
static void handle_time_sync(const comm_event_t *evt)
{
    const comm_msg_time_sync_t *sync = &evt->data.time_sync;

    /* Vérifier que l'émetteur du TIME_SYNC est une autorité de mint reconnue.
     * Un attaquant pourrait autrement injecter de faux timestamps pour
     * désynchroniser les horloges Lamport du réseau. */
    bool is_authorized = false;
    for (uint32_t i = 0; i < s_currency.mint_authority_count; i++) {
        if (public_key_equal(&sync->master_key,
                             &s_currency.mint_authorities[i])) {
            is_authorized = true;
            break;
        }
    }
    if (!is_authorized) {
        ESP_LOGW(TAG, "TIME_SYNC rejete : emetteur non autorise");
        return;
    }

    int ret = time_manager_on_master_sync(&s_time_manager,
                                          &sync->master_key,
                                          sync->master_timestamp,
                                          sync->master_lamport);
    if (ret == 0) {
        ESP_LOGI(TAG, "Time sync accepte");
    } else {
        ESP_LOGD(TAG, "Time sync rejete (%d)", ret);
    }
}

/**
 * @brief Verifie si un broadcast a deja ete vu (via sa signature).
 *
 * Parcourt le cache circulaire s_seen_bcast. La signature Ed25519 est
 * unique par couple (emetteur, contenu), donc suffisante comme identifiant.
 *
 * @return true si deja vu, false sinon
 */
static bool broadcast_already_seen(const signature_t *sig)
{
    uint32_t limit = (s_seen_bcast_count < MAX_SEEN_BROADCASTS)
                         ? s_seen_bcast_count
                         : MAX_SEEN_BROADCASTS;
    for (uint32_t i = 0; i < limit; i++) {
        if (memcmp(s_seen_bcast[i].bytes, sig->bytes,
                   CRYPTO_SIGNATURE_SIZE) == 0) {
            return true;
        }
    }
    return false;
}

/**
 * @brief Ajoute une signature au cache circulaire des broadcasts vus.
 */
static void broadcast_mark_seen(const signature_t *sig)
{
    uint32_t idx = s_seen_bcast_count % MAX_SEEN_BROADCASTS;
    memcpy(&s_seen_bcast[idx], sig, sizeof(signature_t));
    s_seen_bcast_count++;
}

/**
 * @brief Traite la reception d'un broadcast texte maitre via LoRa.
 *
 * Pipeline :
 * 1. Verifier dans le seen cache (anti-boucle relay)
 * 2. La cle publique de l'emetteur doit etre dans mint_authorities
 * 3. La signature Ed25519 du texte doit etre valide
 * 4. Stocker pour l'UI + preparer le relay LoRa
 */
static void handle_broadcast_received(const comm_event_t *evt)
{
    const comm_msg_broadcast_t *bcast = &evt->data.broadcast;

    /* 1. Anti-boucle : ignorer les broadcasts deja traites */
    if (broadcast_already_seen(&bcast->signature)) {
        ESP_LOGD(TAG, "Broadcast deja vu, ignore");
        return;
    }

    /* 2. Verifier que l'emetteur est un maitre connu (mint_authority) */
    bool is_master = false;
    for (uint32_t i = 0; i < s_currency.mint_authority_count; i++) {
        if (public_key_equal(&bcast->sender_key,
                             &s_currency.mint_authorities[i])) {
            is_master = true;
            break;
        }
    }
    if (!is_master) {
        ESP_LOGW(TAG, "Broadcast ignore : emetteur non autorise");
        return;
    }

    /*
     * 3. Verifier la signature Ed25519.
     * La signature couvre [text_len:1][text:N] — on reconstitue
     * ce buffer pour la verification.
     */
    uint8_t signed_data[1 + COMM_MSG_BROADCAST_TEXT_MAX];
    signed_data[0] = bcast->text_len;
    memcpy(&signed_data[1], bcast->text, bcast->text_len);
    size_t signed_len = 1 + bcast->text_len;

    esp_err_t ret = crypto_verify(signed_data, signed_len,
                                  &bcast->sender_key, &bcast->signature);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Broadcast ignore : signature invalide");
        return;
    }

    /* Marquer comme vu dans le cache (avant relay, pour eviter les boucles) */
    broadcast_mark_seen(&bcast->signature);

    /* 4. Stocker le message et activer le flag pour l'UI */
    memcpy(&s_pending_broadcast, bcast, sizeof(comm_msg_broadcast_t));
    s_broadcast_pending = true;

    ESP_LOGI(TAG, "Broadcast maitre accepte (%u chars): \"%.*s\"",
             bcast->text_len, bcast->text_len, bcast->text);

#ifdef MP_HAS_LORA
    /*
     * 5. Preparer le relay LoRa.
     * On re-packe le message original (meme pubkey + sig du maitre)
     * dans un buffer. Le relay effectif se fera apres release du mutex
     * dans core_task, avec un delai aleatoire pour eviter les collisions.
     */
    if (comm_msg_pack_broadcast(s_relay_bcast_buf, sizeof(s_relay_bcast_buf),
                                &bcast->sender_key, &bcast->signature,
                                bcast->text, bcast->text_len,
                                &s_relay_bcast_len) == 0) {
        s_relay_bcast_pending = true;
    }
#endif
}

/* ================================================================
 * Handlers ping/pong
 * ================================================================ */

/**
 * @brief Verifie si un PING a deja ete vu (anti-boucle relay).
 */
static bool ping_already_seen(const public_key_t *master_key, uint16_t ping_id)
{
    uint32_t limit = (s_seen_ping_count < MAX_SEEN_PINGS)
                         ? s_seen_ping_count
                         : MAX_SEEN_PINGS;
    for (uint32_t i = 0; i < limit; i++) {
        if (s_seen_pings[i].ping_id == ping_id &&
            public_key_equal(&s_seen_pings[i].master_key, master_key)) {
            return true;
        }
    }
    return false;
}

/**
 * @brief Marque un PING comme vu dans le cache circulaire.
 */
static void ping_mark_seen(const public_key_t *master_key, uint16_t ping_id)
{
    uint32_t idx = s_seen_ping_count % MAX_SEEN_PINGS;
    memcpy(&s_seen_pings[idx].master_key, master_key, sizeof(public_key_t));
    s_seen_pings[idx].ping_id = ping_id;
    s_seen_ping_count++;
}

/**
 * @brief Traite la reception d'un PING maitre via LoRa.
 *
 * Actions :
 * 1. Verifier le seen cache (anti-boucle)
 * 2. Ignorer si c'est notre propre PING
 * 3. Envoyer un PONG avec notre identite (delai aleatoire 1-5s)
 * 4. Preparer le relay du PING (single-hop)
 */
static void handle_ping_received(const comm_event_t *evt)
{
    const comm_msg_ping_t *ping = &evt->data.ping;

    /* 0. Vérifier que l'émetteur du PING est une autorité de mint reconnue.
     * Seuls les maitres (mint authorities) sont habilités à émettre des PINGs
     * pour le mécanisme de découverte réseau. */
    bool is_authorized = false;
    for (uint32_t i = 0; i < s_currency.mint_authority_count; i++) {
        if (public_key_equal(&ping->master_key,
                             &s_currency.mint_authorities[i])) {
            is_authorized = true;
            break;
        }
    }
    if (!is_authorized) {
        ESP_LOGW(TAG, "PING rejete : emetteur non autorise");
        return;
    }

    /* 1. Anti-boucle : ignorer les PINGs deja vus */
    if (ping_already_seen(&ping->master_key, ping->ping_id)) {
        ESP_LOGD(TAG, "Ping deja vu (id=%u), ignore", ping->ping_id);
        return;
    }

    /* 2. Ignorer notre propre PING (si on est maitre et qu'on l'a emis) */
    if (public_key_equal(&ping->master_key, &s_keypair.public_key)) {
        return;
    }

    /* Marquer comme vu */
    ping_mark_seen(&ping->master_key, ping->ping_id);

    ESP_LOGI(TAG, "Ping maitre recu (id=%u), envoi PONG", ping->ping_id);

#ifdef MP_HAS_LORA
    /*
     * 3. Preparer le PONG avec notre identite (envoi differe).
     *
     * Au lieu de bloquer avec vTaskDelay sous le mutex (ce qui gelait
     * l'UI pendant 1-5 secondes), on stocke le PONG dans un buffer
     * et on note le delai aleatoire. L'envoi effectif se fera hors
     * mutex dans la boucle core_task une fois le delai ecoule.
     */
    {
        /*
         * [I4-fix] Signer le PONG pour empecher l'usurpation d'identite.
         * On signe [ping_id:2 BE][alias_len:1][alias:N] avec la cle
         * privee du device. Le recepteur verifiera la signature contre
         * device_key — si un attaquant forge un PONG avec une autre
         * pubkey que la sienne, il ne pourra pas produire une signature
         * valide.
         */
        uint8_t sign_buf[2 + 1 + COMM_MSG_ALIAS_MAX];
        size_t sign_len = 0;
        sign_buf[sign_len++] = (uint8_t)(ping->ping_id >> 8);
        sign_buf[sign_len++] = (uint8_t)(ping->ping_id);
        sign_buf[sign_len++] = s_device_alias_len;
        memcpy(&sign_buf[sign_len], s_device_alias, s_device_alias_len);
        sign_len += s_device_alias_len;

        signature_t pong_sig;
        if (crypto_sign(sign_buf, sign_len, &s_keypair, &pong_sig) != ESP_OK) {
            ESP_LOGE(TAG, "Erreur signature PONG");
        } else {
            size_t pong_len;
            if (comm_msg_pack_pong(s_pong_buf, sizeof(s_pong_buf),
                                   &s_keypair.public_key,
                                   &pong_sig,
                                   ping->ping_id,
                                   s_device_alias, s_device_alias_len,
                                   &pong_len) == 0) {
                s_pong_len = pong_len;
                s_pong_delay_ms = 1000 + (esp_random() % 4001);
                s_pong_start_tick = xTaskGetTickCount();
                s_pong_pending = true;
                ESP_LOGI(TAG, "PONG signe prepare (envoi differe dans %lums)",
                         (unsigned long)s_pong_delay_ms);
            }
        }
    }

    /* 4. Preparer le relay du PING (on reutilise la signature originale du maitre) */
    if (comm_msg_pack_ping(s_relay_ping_buf, sizeof(s_relay_ping_buf),
                           &ping->master_key, &ping->signature,
                           ping->ping_id, &s_relay_ping_len) == 0) {
        s_relay_ping_pending = true;
    }
#endif
}

/**
 * @brief Traite la reception d'un PONG via LoRa.
 *
 * Collecte les resultats de ping : ajoute le device repondant
 * a la liste si la session correspond et qu'il n'est pas deja present.
 */
static void handle_pong_received(const comm_event_t *evt)
{
    const comm_msg_pong_t *pong = &evt->data.pong;

    /* Ignorer si pas de session ping active ou ping_id different */
    if (!s_ping_active || pong->ping_id != s_current_ping_id) {
        ESP_LOGD(TAG, "PONG ignore (session inactive ou id mismatch)");
        return;
    }

    /* Ignorer si c'est notre propre PONG */
    if (public_key_equal(&pong->device_key, &s_keypair.public_key)) {
        return;
    }

    /* Verifier si ce device est deja dans les resultats */
    for (uint32_t i = 0; i < s_ping_result_count; i++) {
        if (public_key_equal(&s_ping_results[i].key, &pong->device_key)) {
            return; /* Deja connu */
        }
    }

    /* Ajouter aux resultats */
    if (s_ping_result_count >= MAX_PING_RESULTS) {
        ESP_LOGW(TAG, "Table ping pleine, PONG ignore");
        return;
    }

    memcpy(&s_ping_results[s_ping_result_count].key,
           &pong->device_key, sizeof(public_key_t));
    memcpy(s_ping_results[s_ping_result_count].alias,
           pong->alias, pong->alias_len);
    s_ping_results[s_ping_result_count].alias[pong->alias_len] = '\0';
    s_ping_results[s_ping_result_count].alias_len = pong->alias_len;
    s_ping_result_count++;

    ESP_LOGI(TAG, "PONG recu : \"%s\" (%"PRIu32"/%d devices)",
             pong->alias, s_ping_result_count, MAX_PING_RESULTS);
}

/**
 * @brief Traite la reception d'un SET_ALIAS distant via LoRa.
 *
 * Verifie que :
 * 1. L'emetteur est un maitre connu (mint_authority)
 * 2. La signature Ed25519 est valide (couvre target_key + alias_len + alias)
 * 3. La target_key correspond a notre propre cle publique
 *
 * Si toutes les verifications passent, met a jour l'alias en RAM et en NVS.
 */
static void handle_set_alias_received(const comm_event_t *evt)
{
    const comm_msg_set_alias_t *sa = &evt->data.set_alias;

    /* Securite [M13] : verifier que alias_len ne depasse pas la taille
     * maximale autorisee pour eviter un debordement de buffer */
    if (sa->alias_len > COMM_MSG_ALIAS_MAX) {
        ESP_LOGW(TAG, "SET_ALIAS ignore : alias_len=%u depasse le max %u",
                 sa->alias_len, COMM_MSG_ALIAS_MAX);
        return;
    }

    /* 1. Verifier que l'emetteur est un maitre connu */
    bool is_master = false;
    for (uint32_t i = 0; i < s_currency.mint_authority_count; i++) {
        if (public_key_equal(&sa->master_key,
                             &s_currency.mint_authorities[i])) {
            is_master = true;
            break;
        }
    }
    if (!is_master) {
        ESP_LOGW(TAG, "SET_ALIAS ignore : emetteur non autorise");
        return;
    }

    /*
     * 2. Verifier la signature Ed25519.
     * La signature couvre [target_key:32][alias_len:1][alias:N]
     */
    uint8_t signed_data[CRYPTO_PUBLIC_KEY_SIZE + 1 + COMM_MSG_ALIAS_MAX];
    memcpy(signed_data, sa->target_key.bytes, CRYPTO_PUBLIC_KEY_SIZE);
    signed_data[CRYPTO_PUBLIC_KEY_SIZE] = sa->alias_len;
    memcpy(&signed_data[CRYPTO_PUBLIC_KEY_SIZE + 1], sa->alias, sa->alias_len);
    size_t signed_len = CRYPTO_PUBLIC_KEY_SIZE + 1 + sa->alias_len;

    esp_err_t ret = crypto_verify(signed_data, signed_len,
                                  &sa->master_key, &sa->signature);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "SET_ALIAS ignore : signature invalide");
        return;
    }

    /* 3. Verifier que la cible est bien ce device */
    if (!public_key_equal(&sa->target_key, &s_keypair.public_key)) {
        ESP_LOGD(TAG, "SET_ALIAS ignore : cible differente");
        return;
    }

    /* 4. Mettre a jour l'alias en RAM */
    memcpy(s_device_alias, sa->alias, sa->alias_len);
    s_device_alias[sa->alias_len] = '\0';
    s_device_alias_len = sa->alias_len;

    /* 5. Persister en NVS */
    hal_err_t herr = s_storage.blob_write(NVS_NAMESPACE, NVS_KEY_ALIAS,
                                          (const uint8_t *)s_device_alias,
                                          s_device_alias_len, s_storage.ctx);
    if (herr != HAL_OK) {
        ESP_LOGW(TAG, "Echec sauvegarde alias NVS (err=%d)", herr);
    }

    ESP_LOGI(TAG, "Alias mis a jour par le maitre : \"%s\"", s_device_alias);
}

/* ================================================================
 * Handler SET_BENEFICIARY
 * ================================================================ */

/*
 * public_key_is_zero() est fourni par crypto/crypto_types.h (static inline).
 * Ancienne version locale supprimée pour éviter la redéfinition.
 */

/**
 * @brief Traite la reception d'un SET_BENEFICIARY via LoRa.
 *
 * Verifie que :
 * 1. L'emetteur est un maitre connu
 * 2. La signature Ed25519 est valide (couvre target_key + beneficiary_key + interval)
 * 3. La target_key correspond a notre propre cle publique
 *
 * Si beneficiary_key est all-zeros, desactive le mode auto-forward.
 * Sinon, active le forward periodique vers le beneficiaire.
 */
static void handle_set_beneficiary_received(const comm_event_t *evt)
{
    const comm_msg_set_beneficiary_t *sb = &evt->data.set_beneficiary;

    /* 1. Verifier que l'emetteur est un maitre connu */
    bool is_master = false;
    for (uint32_t i = 0; i < s_currency.mint_authority_count; i++) {
        if (public_key_equal(&sb->master_key,
                             &s_currency.mint_authorities[i])) {
            is_master = true;
            break;
        }
    }
    if (!is_master) {
        ESP_LOGW(TAG, "SET_BENEFICIARY ignore : emetteur non autorise");
        return;
    }

    /*
     * 2. Verifier la signature Ed25519.
     * La signature couvre [target_key:32][beneficiary_key:32][interval:2 BE]
     */
    uint8_t signed_data[CRYPTO_PUBLIC_KEY_SIZE + CRYPTO_PUBLIC_KEY_SIZE + 2];
    size_t offset = 0;
    memcpy(&signed_data[offset], sb->target_key.bytes, CRYPTO_PUBLIC_KEY_SIZE);
    offset += CRYPTO_PUBLIC_KEY_SIZE;
    memcpy(&signed_data[offset], sb->beneficiary_key.bytes, CRYPTO_PUBLIC_KEY_SIZE);
    offset += CRYPTO_PUBLIC_KEY_SIZE;
    signed_data[offset]     = (uint8_t)(sb->forward_interval_min >> 8);
    signed_data[offset + 1] = (uint8_t)(sb->forward_interval_min);

    esp_err_t ret = crypto_verify(signed_data, sizeof(signed_data),
                                  &sb->master_key, &sb->signature);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "SET_BENEFICIARY ignore : signature invalide");
        return;
    }

    /* 3. Verifier que la cible est bien ce device */
    if (!public_key_equal(&sb->target_key, &s_keypair.public_key)) {
        ESP_LOGD(TAG, "SET_BENEFICIARY ignore : cible differente");
        return;
    }

    /* 4. Appliquer la configuration */
    if (public_key_is_zero(&sb->beneficiary_key)) {
        /* Desactivation du mode auto-forward */
        memset(&s_beneficiary_key, 0, sizeof(public_key_t));
        s_forward_interval_min = 0;

        /* Nettoyer NVS */
        s_storage.blob_write(NVS_NAMESPACE, NVS_KEY_BENEFICIARY,
                             s_beneficiary_key.bytes, CRYPTO_PUBLIC_KEY_SIZE,
                             s_storage.ctx);
        s_storage.u32_write(NVS_NAMESPACE, NVS_KEY_FWD_INTERVAL,
                            0, s_storage.ctx);

        ESP_LOGI(TAG, "Auto-forward desactive par le maitre");
    } else {
        /* Activation / mise a jour du mode auto-forward */
        memcpy(&s_beneficiary_key, &sb->beneficiary_key, sizeof(public_key_t));
        s_forward_interval_min = sb->forward_interval_min;

        /* Plancher a 1 minute pour eviter le spam de TX */
        if (s_forward_interval_min < 1) {
            s_forward_interval_min = 1;
        }

        /* Persister en NVS */
        s_storage.blob_write(NVS_NAMESPACE, NVS_KEY_BENEFICIARY,
                             s_beneficiary_key.bytes, CRYPTO_PUBLIC_KEY_SIZE,
                             s_storage.ctx);
        s_storage.u32_write(NVS_NAMESPACE, NVS_KEY_FWD_INTERVAL,
                            (uint32_t)s_forward_interval_min, s_storage.ctx);

        /* Reinitialiser le timer pour ne pas forward immediatement */
        s_last_forward_ms = get_time_ms_wrapper();

        ESP_LOGI(TAG, "Auto-forward active : interval=%u min",
                 s_forward_interval_min);
    }
}

/* ================================================================
 * Auto-forward beneficiaire
 * ================================================================ */

/**
 * @brief Tente un transfert automatique du solde vers le beneficiaire.
 *
 * Appelee periodiquement sous le mutex s_state_mutex quand le mode
 * auto-forward est actif (s_forward_interval_min > 0).
 *
 * Cree une TX TRANSFER auto-confirmee (pas d'ACK ESP-NOW necessaire).
 * La TX sera propagee au reseau via la LoRa sync.
 */
static void attempt_beneficiary_forward(void)
{
    /* Calculer le solde disponible (checkpoint + DAG, avec fonte appliquee) */
    uint32_t available = 0;
    compute_owner_balance(&available);
    apply_pending_melt(&available);

    uint32_t total_locked = 0;
    lock_table_total_locked(&s_lock_table, &total_locked);

    uint32_t spendable = (available > total_locked) ? available - total_locked : 0;

    /* Le forward doit couvrir les frais de transfert */
    if (spendable <= s_currency.transfer_fee) {
        ESP_LOGD(TAG, "Forward: solde insuffisant (%"PRIu32" <= fee %"PRIu32")",
                 spendable, s_currency.transfer_fee);
        return;
    }

    uint32_t forward_amount = spendable - s_currency.transfer_fee;

    /* Respecter le montant minimum de transfert si defini */
    if (s_currency.min_transfer_amount > 0 &&
        forward_amount < s_currency.min_transfer_amount) {
        ESP_LOGD(TAG, "Forward: montant trop faible (%"PRIu32" < min %"PRIu32")",
                 forward_amount, s_currency.min_transfer_amount);
        return;
    }

    /* Respecter le montant maximum de transfert si defini */
    if (s_currency.max_transfer_amount > 0 &&
        forward_amount > s_currency.max_transfer_amount) {
        forward_amount = s_currency.max_transfer_amount;
    }

    /* Recuperer les tips du DAG comme parents */
    const transaction_t *tips[2];
    uint32_t tip_count = 0;
    dag_get_tips(&s_dag, tips, 2, &tip_count);

    if (tip_count == 0) {
        ESP_LOGW(TAG, "Forward: pas de tips dans le DAG");
        return;
    }

    hash_t parents[2];
    uint8_t parent_count = (tip_count > 2) ? 2 : (uint8_t)tip_count;
    for (uint8_t i = 0; i < parent_count; i++) {
        memcpy(&parents[i], &tips[i]->id, sizeof(hash_t));
    }

    /* Creer la TX de transfert */
    uint64_t timestamp = get_tx_timestamp_wrapper();

    transaction_t tx;
    esp_err_t ret = tx_create_transfer(&tx, &s_keypair, &s_beneficiary_key,
                                       forward_amount, s_currency.currency_id,
                                       s_currency.transfer_fee,
                                       next_seq(),
                                       parents, parent_count, timestamp);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Forward: erreur creation TX: %d", ret);
        return;
    }

    /*
     * Auto-confirmation : le device s'envoie a lui-meme la confirmation.
     * Le champ status n'est pas couvert par la signature, donc on peut
     * le modifier apres creation. Meme pattern que les TX MINT.
     */
    tx.status = TX_STATUS_CONFIRMED;

    /* Inserer dans le DAG (pas de verrouillage, deja CONFIRMED) */
    ret = dag_insert_and_track(&tx);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Forward: erreur insertion DAG: %d", ret);
        return;
    }

    ESP_LOGI(TAG, "Auto-forward: %"PRIu32" credits transferes au beneficiaire",
             forward_amount);
}

/**
 * @brief Verifie les verrous expires et annule les TX correspondantes.
 *
 * Appelee periodiquement dans la boucle core_task.
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
 * Fonction de paiement (appelee depuis l'UI)
 * ================================================================ */

/**
 * @brief Initie un paiement vers un destinataire.
 *
 * [C1-fix] Cette fonction DOIT etre appelee avec s_state_mutex deja pris
 * par l'appelant (core_task). Comme le mutex s_state_mutex n'est pas
 * recursif (xSemaphoreCreateMutex), le reprendre ici depuis un contexte
 * deja locke provoquait un deadlock/timeout systematique sur les paiements
 * initiates depuis l'UI.
 *
 * Les seuls appelants sont handle_ui_command() (sous mutex) : aucune
 * exposition publique n'est necessaire.
 *
 * @param to     Cle publique du destinataire
 * @param amount Montant a envoyer
 * @return ESP_OK si le paiement a ete initie avec succes
 */
static esp_err_t initiate_payment(const public_key_t *to, uint32_t amount)
{
    if (to == NULL || amount == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = ESP_FAIL;

    /* 1. Verifier le solde disponible (checkpoint + DAG + fonte appliquee) */
    uint32_t available = 0;
    compute_owner_balance(&available);
    apply_pending_melt(&available);

    uint32_t total_locked = 0;
    lock_table_total_locked(&s_lock_table, &total_locked);

    uint32_t spendable = (available > total_locked) ? available - total_locked : 0;
    uint32_t total_cost = amount + s_currency.transfer_fee;

    if (spendable < total_cost) {
        ESP_LOGW(TAG, "Solde insuffisant: dispo=%"PRIu32" requis=%"PRIu32,
                 spendable, total_cost);
        ret = ESP_ERR_INVALID_STATE;
        goto done;
    }

    /* 2. Recuperer les tips du DAG comme parents */
    const transaction_t *tips[2];
    uint32_t tip_count = 0;
    dag_get_tips(&s_dag, tips, 2, &tip_count);

    if (tip_count == 0) {
        ESP_LOGE(TAG, "Pas de tips dans le DAG");
        ret = ESP_ERR_INVALID_STATE;
        goto done;
    }

    hash_t parents[2];
    uint8_t parent_count = (tip_count > 2) ? 2 : (uint8_t)tip_count;
    for (uint8_t i = 0; i < parent_count; i++) {
        memcpy(&parents[i], &tips[i]->id, sizeof(hash_t));
    }

    /* 3. Creer la transaction */
    uint64_t timestamp = get_tx_timestamp_wrapper();

    transaction_t tx;
    ret = tx_create_transfer(&tx, &s_keypair, to, amount,
                             s_currency.currency_id,
                             s_currency.transfer_fee,
                             next_seq(),
                             parents, parent_count, timestamp);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Erreur creation TX: %d", ret);
        goto done;
    }

    /* 4. Inserer dans le DAG */
    ret = dag_insert_and_track(&tx);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Erreur insertion DAG: %d", ret);
        goto done;
    }

    /* 5. Verrouiller le montant */
    ret = lock_table_lock(&s_lock_table, &tx.id, amount);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Erreur verrouillage: %d", ret);
        goto done;
    }

#ifdef MP_HAS_ESPNOW
    /* 6. Envoyer via ESP-NOW */
    const uint8_t *dest_mac = find_peer_mac(to);
    if (dest_mac != NULL) {
        comm_cmd_t cmd;
        memset(&cmd, 0, sizeof(cmd));
        cmd.type = COMM_CMD_SEND_TX;
        memcpy(&cmd.data.send_tx.tx, &tx, sizeof(transaction_t));
        memcpy(cmd.data.send_tx.dest_mac, dest_mac, 6);
        xQueueSend(s_cmd_queue, &cmd, 0);
    } else {
        ESP_LOGW(TAG, "Destinataire non trouve dans la table des peers");
        /* La TX est quand meme dans le DAG + lockee, elle sera sync via LoRa
         * sur les cartes equipees de LoRa. Sur les autres (Waveshare S3),
         * elle restera dans le DAG local jusqu'a une future decouverte. */
    }
#endif

    ESP_LOGI(TAG, "Paiement initie: amount=%"PRIu32, amount);
    ret = ESP_OK;

done:
    /* [C1-fix] Pas de xSemaphoreGive : le mutex appartient a l'appelant (core_task) */
    return ret;
}

/* ================================================================
 * Creation de credits (MINT, maitre uniquement)
 * ================================================================ */

/**
 * @brief Cree des credits pour un destinataire (MINT).
 *
 * Seul un device maitre (present dans mint_authorities) peut MINT.
 * La TX est inseree dans le DAG et sera propagee via LoRa sync.
 *
 * @param to     Cle publique du destinataire
 * @param amount Montant a creer
 * @return ESP_OK si succes
 */
static esp_err_t initiate_mint(const public_key_t *to, uint32_t amount)
{
    if (to == NULL || amount == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Verifier que ce device est maitre */
    bool is_master = false;
    for (uint8_t i = 0; i < s_currency.mint_authority_count; i++) {
        if (memcmp(s_keypair.public_key.bytes,
                   s_currency.mint_authorities[i].bytes,
                   CRYPTO_PUBLIC_KEY_SIZE) == 0) {
            is_master = true;
            break;
        }
    }
    if (!is_master) {
        ESP_LOGW(TAG, "MINT refuse : device non maitre");
        return ESP_ERR_NOT_ALLOWED;
    }

    /* [C1-fix] Pas de xSemaphoreTake : mutex deja pris par core_task (appelant unique) */

    esp_err_t ret;

    /* Recuperer les tips du DAG comme parents */
    const transaction_t *tips[2];
    uint32_t tip_count = 0;
    dag_get_tips(&s_dag, tips, 2, &tip_count);

    hash_t parents[2];
    uint8_t parent_count;

    if (tip_count == 0) {
        /* DAG vide : utiliser un parent genese */
        memset(&parents[0], 0, sizeof(hash_t));
        parent_count = 1;
    } else {
        parent_count = (tip_count > 2) ? 2 : (uint8_t)tip_count;
        for (uint8_t i = 0; i < parent_count; i++) {
            memcpy(&parents[i], &tips[i]->id, sizeof(hash_t));
        }
    }

    /* Creer la transaction MINT */
    transaction_t mint_tx;
    ret = tx_create_mint(&mint_tx, &s_keypair, to, amount,
                         s_currency.currency_id,
                         next_seq(),
                         parents, parent_count,
                         get_tx_timestamp_wrapper());
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Erreur creation MINT: %s", esp_err_to_name(ret));
        /* [C1-fix] Pas de xSemaphoreGive : mutex appartient a l'appelant */
        return ret;
    }

    /* Inserer dans le DAG et suivre pour checkpoint automatique */
    ret = dag_insert_and_track(&mint_tx);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Erreur insertion MINT dans DAG: %s",
                 esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "MINT cree: %"PRIu32" credits", amount);
    }

    /* [C1-fix] Pas de xSemaphoreGive : mutex appartient a l'appelant (core_task) */
    return ret;
}

/* ================================================================
 * Envoi de broadcast texte (maitre uniquement)
 * ================================================================ */

/**
 * @brief Envoie un broadcast texte signe a tous les devices LoRa.
 *
 * Seul un device maitre (present dans mint_authorities) peut envoyer.
 * Le texte est signe avec la cle privee du device pour authentification.
 *
 * Thread-safe : pas d'acces a l'etat partage (pas de mutex necessaire).
 *
 * @param text     Texte a envoyer (null-terminated)
 * @param text_len Longueur du texte (1..COMM_MSG_BROADCAST_TEXT_MAX)
 * @return ESP_OK si envoye, ESP_ERR_INVALID_ARG si parametres invalides,
 *         ESP_ERR_NOT_ALLOWED si le device n'est pas maitre
 */
#if CONFIG_IDF_TARGET_ESP32
esp_err_t broadcast_text_send(const char *text, uint8_t text_len)
{
    if (!text || text_len == 0 || text_len > COMM_MSG_BROADCAST_TEXT_MAX) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Verifier que ce device est un maitre */
    bool is_master = false;
    for (uint32_t i = 0; i < s_currency.mint_authority_count; i++) {
        if (public_key_equal(&s_keypair.public_key,
                             &s_currency.mint_authorities[i])) {
            is_master = true;
            break;
        }
    }
    if (!is_master) {
        ESP_LOGW(TAG, "broadcast_text_send: device non maitre");
        return ESP_ERR_NOT_ALLOWED;
    }

    /*
     * Signer [text_len:1][text:N] avec la cle privee du device.
     * Ce format correspond a ce que le recepteur verifiera.
     */
    uint8_t signed_data[1 + COMM_MSG_BROADCAST_TEXT_MAX];
    signed_data[0] = text_len;
    memcpy(&signed_data[1], text, text_len);
    size_t signed_len = 1 + text_len;

    signature_t sig;
    esp_err_t ret = crypto_sign(signed_data, signed_len, &s_keypair, &sig);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Erreur signature broadcast: %d", ret);
        return ret;
    }

    /* Construire le message dans un buffer */
    uint8_t buf[COMM_MSG_LORA_MAX];
    size_t out_len;
    if (comm_msg_pack_broadcast(buf, sizeof(buf),
                                &s_keypair.public_key, &sig,
                                text, text_len, &out_len) != 0) {
        ESP_LOGE(TAG, "Erreur pack broadcast");
        return ESP_FAIL;
    }

    /* Envoyer via LoRa */
    hal_err_t herr = s_lora_hal.send(buf, out_len, s_lora_hal.ctx);
    if (herr != HAL_OK) {
        ESP_LOGE(TAG, "Erreur envoi LoRa broadcast: %d", herr);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Broadcast envoye (%u chars): \"%.*s\"",
             text_len, text_len, text);
    return ESP_OK;
}
#endif

/* ================================================================
 * Ping LoRa (maitre uniquement)
 * ================================================================ */

/**
 * @brief Lance un ping LoRa pour decouvrir les devices a portee.
 *
 * Reinitialise les resultats, envoie un PING via LoRa, et active
 * la collecte de PONGs. L'appelant (UI) doit attendre quelques secondes
 * puis consulter s_ping_results[] et s_ping_result_count.
 *
 * @return ESP_OK si envoye, ESP_FAIL si erreur
 */
#if CONFIG_IDF_TARGET_ESP32
esp_err_t ping_send(void)
{
    /* Incrementer le ping_id pour cette session */
    s_current_ping_id++;

    /* Reinitialiser les resultats */
    s_ping_result_count = 0;
    memset(s_ping_results, 0, sizeof(s_ping_results));

    /* Marquer notre propre PING comme vu (pour ne pas le traiter si on le recoit via relay) */
    ping_mark_seen(&s_keypair.public_key, s_current_ping_id);

    /*
     * Signer [ping_id:2 BE] avec la cle privee du device.
     * Ce format correspond a ce que le recepteur verifiera.
     */
    uint8_t sign_buf[2];
    sign_buf[0] = (uint8_t)(s_current_ping_id >> 8);
    sign_buf[1] = (uint8_t)(s_current_ping_id);

    signature_t sig;
    esp_err_t ret = crypto_sign(sign_buf, sizeof(sign_buf), &s_keypair, &sig);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Erreur signature PING: %d", ret);
        return ret;
    }

    /* Construire et envoyer le PING signe */
    uint8_t buf[COMM_MSG_PING_SIZE];
    size_t out_len;
    if (comm_msg_pack_ping(buf, sizeof(buf),
                           &s_keypair.public_key, &sig,
                           s_current_ping_id, &out_len) != 0) {
        ESP_LOGE(TAG, "Erreur pack PING");
        return ESP_FAIL;
    }

    hal_err_t herr = s_lora_hal.send(buf, out_len, s_lora_hal.ctx);
    if (herr != HAL_OK) {
        ESP_LOGE(TAG, "Erreur envoi LoRa PING: %d", herr);
        return ESP_FAIL;
    }

    /* Activer la collecte de PONGs */
    s_ping_active = true;

    ESP_LOGI(TAG, "PING signe envoye (id=%u), collecte PONGs...",
             s_current_ping_id);
    return ESP_OK;
}

/* ================================================================
 * Renommage distant (maitre uniquement)
 * ================================================================ */

/**
 * @brief Envoie un SET_ALIAS via LoRa pour renommer un device distant.
 *
 * Signe [target_key:32][alias_len:1][alias:N] avec la cle du maitre,
 * packe le message et l'envoie via LoRa. Le message n'est pas relaye.
 *
 * @param target_key Cle publique du device cible
 * @param alias      Nouvel alias (null-terminated)
 * @param alias_len  Longueur du nouvel alias (1..COMM_MSG_ALIAS_MAX)
 * @return ESP_OK si envoye, ESP_FAIL si erreur
 */
esp_err_t set_alias_send(const public_key_t *target_key,
                         const char *alias, uint8_t alias_len)
{
    if (!target_key || !alias || alias_len == 0 || alias_len > COMM_MSG_ALIAS_MAX) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Verifier que ce device est un maitre */
    bool is_master = false;
    for (uint32_t i = 0; i < s_currency.mint_authority_count; i++) {
        if (public_key_equal(&s_keypair.public_key,
                             &s_currency.mint_authorities[i])) {
            is_master = true;
            break;
        }
    }
    if (!is_master) {
        ESP_LOGW(TAG, "set_alias_send: device non maitre");
        return ESP_ERR_NOT_ALLOWED;
    }

    /*
     * Signer [target_key:32][alias_len:1][alias:N] avec la cle privee.
     * Ce format correspond a ce que le recepteur verifiera.
     */
    uint8_t signed_data[CRYPTO_PUBLIC_KEY_SIZE + 1 + COMM_MSG_ALIAS_MAX];
    memcpy(signed_data, target_key->bytes, CRYPTO_PUBLIC_KEY_SIZE);
    signed_data[CRYPTO_PUBLIC_KEY_SIZE] = alias_len;
    memcpy(&signed_data[CRYPTO_PUBLIC_KEY_SIZE + 1], alias, alias_len);
    size_t signed_len = CRYPTO_PUBLIC_KEY_SIZE + 1 + alias_len;

    signature_t sig;
    esp_err_t ret = crypto_sign(signed_data, signed_len, &s_keypair, &sig);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Erreur signature SET_ALIAS: %d", ret);
        return ret;
    }

    /* Construire le message dans un buffer */
    uint8_t buf[COMM_MSG_LORA_MAX];
    size_t out_len;
    if (comm_msg_pack_set_alias(buf, sizeof(buf),
                                &s_keypair.public_key, &sig,
                                target_key, alias, alias_len,
                                &out_len) != 0) {
        ESP_LOGE(TAG, "Erreur pack SET_ALIAS");
        return ESP_FAIL;
    }

    /* Envoyer via LoRa */
    hal_err_t herr = s_lora_hal.send(buf, out_len, s_lora_hal.ctx);
    if (herr != HAL_OK) {
        ESP_LOGE(TAG, "Erreur envoi LoRa SET_ALIAS: %d", herr);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "SET_ALIAS envoye : \"%.*s\"", alias_len, alias);
    return ESP_OK;
}

/* ================================================================
 * Configuration beneficiaire (maitre uniquement)
 * ================================================================ */

/**
 * @brief Envoie un SET_BENEFICIARY via LoRa pour configurer l'auto-forward.
 *
 * Signe [target_key:32][beneficiary_key:32][interval:2 BE] avec la cle
 * du maitre, packe le message et l'envoie via LoRa.
 *
 * Pour desactiver le mode, passer une beneficiary_key all-zeros.
 *
 * @param target_key       Cle publique du device cible
 * @param beneficiary_key  Cle publique du beneficiaire (all-zeros = desactiver)
 * @param forward_interval_min Intervalle en minutes (ignore si desactivation)
 * @return ESP_OK si envoye, ESP_FAIL si erreur
 */
esp_err_t set_beneficiary_send(const public_key_t *target_key,
                               const public_key_t *beneficiary_key,
                               uint16_t forward_interval_min)
{
    if (!target_key || !beneficiary_key) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Verifier que ce device est un maitre */
    bool is_master = false;
    for (uint32_t i = 0; i < s_currency.mint_authority_count; i++) {
        if (public_key_equal(&s_keypair.public_key,
                             &s_currency.mint_authorities[i])) {
            is_master = true;
            break;
        }
    }
    if (!is_master) {
        ESP_LOGW(TAG, "set_beneficiary_send: device non maitre");
        return ESP_ERR_NOT_ALLOWED;
    }

    /*
     * Signer [target_key:32][beneficiary_key:32][interval:2 BE].
     */
    uint8_t signed_data[CRYPTO_PUBLIC_KEY_SIZE + CRYPTO_PUBLIC_KEY_SIZE + 2];
    size_t offset = 0;
    memcpy(&signed_data[offset], target_key->bytes, CRYPTO_PUBLIC_KEY_SIZE);
    offset += CRYPTO_PUBLIC_KEY_SIZE;
    memcpy(&signed_data[offset], beneficiary_key->bytes, CRYPTO_PUBLIC_KEY_SIZE);
    offset += CRYPTO_PUBLIC_KEY_SIZE;
    signed_data[offset]     = (uint8_t)(forward_interval_min >> 8);
    signed_data[offset + 1] = (uint8_t)(forward_interval_min);

    signature_t sig;
    esp_err_t ret = crypto_sign(signed_data, sizeof(signed_data), &s_keypair, &sig);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Erreur signature SET_BENEFICIARY: %d", ret);
        return ret;
    }

    /* Construire le message dans un buffer */
    uint8_t buf[COMM_MSG_SET_BENEFICIARY_SIZE];
    size_t out_len;
    if (comm_msg_pack_set_beneficiary(buf, sizeof(buf),
                                      &s_keypair.public_key, &sig,
                                      target_key, beneficiary_key,
                                      forward_interval_min,
                                      &out_len) != 0) {
        ESP_LOGE(TAG, "Erreur pack SET_BENEFICIARY");
        return ESP_FAIL;
    }

    /* Envoyer via LoRa */
    hal_err_t herr = s_lora_hal.send(buf, out_len, s_lora_hal.ctx);
    if (herr != HAL_OK) {
        ESP_LOGE(TAG, "Erreur envoi LoRa SET_BENEFICIARY: %d", herr);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "SET_BENEFICIARY envoye (interval=%u min)",
             forward_interval_min);
    return ESP_OK;
}
#endif

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
#ifdef MP_HAS_ESPNOW
            {
                comm_cmd_t disc_cmd;
                memset(&disc_cmd, 0, sizeof(disc_cmd));
                disc_cmd.type = COMM_CMD_START_DISCOVER;
                xQueueSend(s_cmd_queue, &disc_cmd, pdMS_TO_TICKS(100));
            }
#else
            ESP_LOGW(TAG, "DISCOVER non disponible sur ce device (pas d'ESP-NOW)");
#endif
            break;

        case UI_CMD_BROADCAST_TEXT:
            ESP_LOGI(TAG, "UI CMD: Broadcast texte (%u chars)",
                     cmd->data.broadcast.text_len);
#if CONFIG_IDF_TARGET_ESP32
            broadcast_text_send(cmd->data.broadcast.text,
                                cmd->data.broadcast.text_len);
#endif
            break;

        case UI_CMD_PING:
            ESP_LOGI(TAG, "UI CMD: Ping");
#if CONFIG_IDF_TARGET_ESP32
            ping_send();
#endif
            break;

        case UI_CMD_SET_ALIAS:
            ESP_LOGI(TAG, "UI CMD: Set alias");
#if CONFIG_IDF_TARGET_ESP32
            set_alias_send(&cmd->data.set_alias.target,
                           cmd->data.set_alias.alias,
                           cmd->data.set_alias.alias_len);
#endif
            break;

        case UI_CMD_SET_BENEFICIARY:
            ESP_LOGI(TAG, "UI CMD: Set beneficiary");
#if CONFIG_IDF_TARGET_ESP32
            set_beneficiary_send(&cmd->data.set_beneficiary.target,
                                 &cmd->data.set_beneficiary.beneficiary,
                                 cmd->data.set_beneficiary.interval_min);
#endif
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

#ifdef MP_HAS_LORA
        /*
         * Relay broadcast LoRa (hors mutex).
         *
         * On attend un delai aleatoire (200-1000ms) avant de retransmettre
         * pour eviter que tous les devices relayent en meme temps et
         * provoquent des collisions sur le canal LoRa.
         */
        if (s_relay_bcast_pending) {
            s_relay_bcast_pending = false;
            uint32_t delay_ms = 200 + (esp_random() % 801);
            vTaskDelay(pdMS_TO_TICKS(delay_ms));
            hal_err_t herr = s_lora_hal.send(s_relay_bcast_buf,
                                              s_relay_bcast_len,
                                              s_lora_hal.ctx);
            if (herr == HAL_OK) {
                ESP_LOGI(TAG, "Broadcast relaye (delai=%lums)",
                         (unsigned long)delay_ms);
            } else {
                ESP_LOGW(TAG, "Echec relay broadcast LoRa");
            }
        }

        /*
         * Relay PING LoRa (hors mutex).
         * Meme principe que le relay broadcast : delai aleatoire
         * pour eviter les collisions.
         */
        if (s_relay_ping_pending) {
            s_relay_ping_pending = false;
            uint32_t delay_ms = 200 + (esp_random() % 801);
            vTaskDelay(pdMS_TO_TICKS(delay_ms));
            hal_err_t herr = s_lora_hal.send(s_relay_ping_buf,
                                              s_relay_ping_len,
                                              s_lora_hal.ctx);
            if (herr == HAL_OK) {
                ESP_LOGI(TAG, "PING relaye (delai=%lums)",
                         (unsigned long)delay_ms);
            } else {
                ESP_LOGW(TAG, "Echec relay PING LoRa");
            }
        }

        /*
         * Envoi differe du PONG (hors mutex).
         *
         * Le PONG a ete prepare dans handle_ping_received() avec un
         * delai aleatoire (1-5s). On verifie ici si le delai est
         * ecoule avant d'envoyer, evitant ainsi de bloquer core_task
         * et le mutex pendant plusieurs secondes.
         */
        if (s_pong_pending) {
            TickType_t elapsed = xTaskGetTickCount() - s_pong_start_tick;
            if (elapsed >= pdMS_TO_TICKS(s_pong_delay_ms)) {
                s_pong_pending = false;
                hal_err_t herr = s_lora_hal.send(s_pong_buf, s_pong_len,
                                                  s_lora_hal.ctx);
                if (herr == HAL_OK) {
                    ESP_LOGI(TAG, "PONG envoye (delai=%lums)",
                             (unsigned long)s_pong_delay_ms);
                } else {
                    ESP_LOGW(TAG, "Echec envoi PONG");
                }
            }
        }
#endif
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

#ifdef MP_HAS_ESPNOW
    if (espnow_hal_esp32_create(&s_espnow_hal) != HAL_OK) {
        ESP_LOGE(TAG, "ESP-NOW HAL init echoue");
        return;
    }
#endif

#ifdef MP_HAS_LORA
    if (hal_lora_wio_e5_create(&s_lora_hal, LORA_UART_NUM,
                                LORA_TX_PIN, LORA_RX_PIN) != HAL_OK) {
        ESP_LOGW(TAG, "LoRa HAL init echoue (fonctionnement sans LoRa)");
    }
#endif

    ESP_LOGI(TAG, "[11/12] HAL initialises"
#ifdef MP_HAS_ESPNOW
             " + ESP-NOW"
#endif
#ifdef MP_HAS_LORA
             " + LoRa"
#endif
            );

    /* ---- 12. Queues, mutex et taches ---- */
    s_evt_queue = xQueueCreate(EVT_QUEUE_DEPTH, sizeof(comm_event_t));
    s_state_mutex = xSemaphoreCreateMutex();

#ifdef MP_HAS_ESPNOW
    s_cmd_queue = xQueueCreate(CMD_QUEUE_DEPTH, sizeof(comm_cmd_t));
    if (!s_cmd_queue) {
        ESP_LOGE(TAG, "Erreur creation s_cmd_queue");
        return;
    }
#endif

    if (!s_evt_queue || !s_state_mutex) {
        ESP_LOGE(TAG, "Erreur creation queues/mutex");
        return;
    }

#ifdef MP_HAS_ESPNOW
    /* Configuration ESP-NOW task */
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
#endif

#ifdef MP_HAS_LORA
    /* Configuration LoRa sync task.
     *
     * [Lot C item 7] Le DAG et le mutex applicatif ne sont plus passes
     * directement au composant : on lui fournit un callback de collecte
     * qui gere lui-meme le verrouillage. Inversion de dependance
     * propre, le composant lora_sync reste decouple. */
    static lora_sync_config_t lora_cfg;
    memset(&lora_cfg, 0, sizeof(lora_cfg));
    lora_cfg.lora = &s_lora_hal;
    lora_cfg.evt_queue = s_evt_queue;
    lora_cfg.collect_confirmed_txs = main_collect_confirmed_txs;
    lora_cfg.collect_ctx = NULL;
    lora_cfg.sync_interval_ms = LORA_SYNC_INTERVAL_MS;
    lora_cfg.is_master = false;
    lora_cfg.get_time = get_time_ms_wrapper;
    lora_cfg.own_pubkey = &s_keypair.public_key;
    lora_cfg.own_keypair = &s_keypair;
    lora_cfg.get_lamport = get_lamport_wrapper;

    xTaskCreate(lora_sync_task, "lora", LORA_TASK_STACK, &lora_cfg,
                LORA_TASK_PRIO, NULL);
#endif

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
