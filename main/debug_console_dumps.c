/**
 * @file debug_console_dumps.c
 * @brief 4 callbacks de dump JSON pour le moniteur multi-device serie.
 *
 * Pattern inversion de dependance : le composant debug_console ne
 * touche jamais aux types applicatifs ni au mutex directement. Chaque
 * callback prend s_state_mutex (timeout court), itere sur l'etat
 * partage, emet une ligne JSON par item via le writer fourni.
 *
 * Mono-thread garanti : tous les callbacks sont appeles en sequence
 * par la meme tache dbg_console.
 *
 * [F-DC-004] Compile uniquement quand `CONFIG_MESHPAY_DEBUG_CONSOLE=y`.
 * Garde-fou au niveau source (en plus du CMake) pour qu'un système de
 * build alternatif (Unity host-based, CI cross direct) ne puisse pas
 * inclure ce fichier sans la garde.
 */

#include "sdkconfig.h"

#if CONFIG_MESHPAY_DEBUG_CONSOLE

#include "debug_console_dumps.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"

#include "app_state.h"
#include "debug_console/debug_console.h"
#include "wallet/wallet.h"

static const char *TAG = "dbg_dumps";

/*
 * Taille du buffer ligne. Alloue sur la stack (la DRAM est saturee,
 * dette technique connue, ~60 octets de marge en .bss).
 *
 * [F-DC-001] Assertion compile-time : la valeur Kconfig doit couvrir
 * le pire cas calculé (TX DAG TRANSFER avec 2 parents) = 497 octets +
 * \0. Sans cette garantie, le dump JSON est silencieusement tronqué
 * pour les TX à 2 parents (cas normal après genesis).
 */
#define DBG_LINE_SIZE CONFIG_MESHPAY_DEBUG_CONSOLE_LINE_BUF_SIZE
_Static_assert(DBG_LINE_SIZE >= 512,
               "[F-DC-001] CONFIG_MESHPAY_DEBUG_CONSOLE_LINE_BUF_SIZE doit "
               "etre >= 512 pour couvrir le pire cas dump DAG (2 parents)");

static inline void dbg_hex(const uint8_t *src, size_t len,
                           char *dst, size_t dst_size)
{
    debug_console_hex_encode(src, len, dst, dst_size);
}

static const char *dbg_status_name(tx_status_t s)
{
    switch (s) {
        case TX_STATUS_LOCKED:    return "LOCKED";
        case TX_STATUS_CONFIRMED: return "CONFIRMED";
        case TX_STATUS_CANCELLED: return "CANCELLED";
        default:                  return "?";
    }
}

/* ----------------------------------------------------------------
 * Dump du DAG
 * ---------------------------------------------------------------- */
static void dump_dag(debug_console_writer_fn writer, void *ctx)
{
    char line[DBG_LINE_SIZE];
    const TickType_t to = pdMS_TO_TICKS(CONFIG_MESHPAY_DEBUG_CONSOLE_MUTEX_TIMEOUT_MS);
    if (xSemaphoreTake(s_state_mutex, to) != pdTRUE) {
        writer("{\"err\":\"mutex_timeout\"}", ctx);
        return;
    }

    snprintf(line, sizeof(line),
             "{\"count\":%lu,\"max\":%lu}",
             (unsigned long)s_dag.count,
             (unsigned long)DAG_MAX_TRANSACTIONS);
    writer(line, ctx);

    char id_hex[CRYPTO_HASH_SIZE * 2 + 1];
    char from_hex[CRYPTO_PUBLIC_KEY_SIZE * 2 + 1];
    char to_hex[CRYPTO_PUBLIC_KEY_SIZE * 2 + 1];
    char parent_hex[CRYPTO_HASH_SIZE * 2 + 1];

    for (uint32_t i = 0; i < s_dag.count; i++) {
        const transaction_t *tx = &s_dag.transactions[i];

        dbg_hex(tx->id.bytes,   sizeof(tx->id.bytes),   id_hex,   sizeof(id_hex));
        dbg_hex(tx->from.bytes, sizeof(tx->from.bytes), from_hex, sizeof(from_hex));
        dbg_hex(tx->to.bytes,   sizeof(tx->to.bytes),   to_hex,   sizeof(to_hex));

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

        /*
         * [F-DC-002] Garde normalisée : snprintf retourne le nombre de
         * caractères qui AURAIENT été écrits sans troncature. Si
         * `n >= sizeof(line)`, le buffer est plein (troncature). On
         * traite ce cas explicitement en émettant un objet d'erreur
         * plutôt qu'un JSON incomplet.
         */
        if (n <= 0 || n >= (int)sizeof(line)) {
            writer("{\"err\":\"line_truncated\"}", ctx);
            continue;
        }

        for (uint8_t p = 0; p < tx->parent_count; p++) {
            dbg_hex(tx->parents[p].bytes, sizeof(tx->parents[p].bytes),
                    parent_hex, sizeof(parent_hex));
            int rem = (int)sizeof(line) - n;
            int written = snprintf(line + n, (size_t)rem,
                                   "%s\"%s\"", p > 0 ? "," : "", parent_hex);
            if (written <= 0 || written >= rem) {
                /* Troncature pendant l'écriture des parents : abandon. */
                writer("{\"err\":\"line_truncated\"}", ctx);
                n = -1;
                break;
            }
            n += written;
        }
        if (n > 0) {
            int rem = (int)sizeof(line) - n;
            int written = snprintf(line + n, (size_t)rem, "]}");
            if (written > 0 && written < rem) {
                writer(line, ctx);
            } else {
                writer("{\"err\":\"line_truncated\"}", ctx);
            }
        }
    }

    xSemaphoreGive(s_state_mutex);
}

/* ----------------------------------------------------------------
 * Dump du wallet (identite + locks actifs)
 * ---------------------------------------------------------------- */
static void dump_wallet(debug_console_writer_fn writer, void *ctx)
{
    char line[DBG_LINE_SIZE];
    const TickType_t to = pdMS_TO_TICKS(CONFIG_MESHPAY_DEBUG_CONSOLE_MUTEX_TIMEOUT_MS);
    if (xSemaphoreTake(s_state_mutex, to) != pdTRUE) {
        writer("{\"err\":\"mutex_timeout\"}", ctx);
        return;
    }

    /*
     * [F-DC-003] On vérifie le retour pour logger un warning si erreur.
     * Le mutex reste détenu après l'appel ; en cas d'erreur, on
     * continue avec balance = 0 plutôt que de laisser le retour en
     * (void) qui masquerait silencieusement le problème.
     */
    uint32_t balance = 0;
    esp_err_t bal_err = wallet_get_balance_for(&s_dag, &s_checkpoint,
                                                &s_keypair.public_key,
                                                &s_wallet.fee_recipient,
                                                &balance);
    if (bal_err != ESP_OK) {
        ESP_LOGW(TAG, "wallet_get_balance_for err=0x%x, balance=0 dans le dump",
                 (int)bal_err);
    }

    char own_hex[CRYPTO_PUBLIC_KEY_SIZE * 2 + 1];
    char fee_hex[CRYPTO_PUBLIC_KEY_SIZE * 2 + 1];
    dbg_hex(s_keypair.public_key.bytes, sizeof(s_keypair.public_key.bytes),
            own_hex, sizeof(own_hex));
    dbg_hex(s_wallet.fee_recipient.bytes, sizeof(s_wallet.fee_recipient.bytes),
            fee_hex, sizeof(fee_hex));

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

/* ----------------------------------------------------------------
 * Dump de la config monnaie
 * ---------------------------------------------------------------- */
static void dump_currency(debug_console_writer_fn writer, void *ctx)
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

/* ----------------------------------------------------------------
 * Dump du time_manager
 * ---------------------------------------------------------------- */
static void dump_time(debug_console_writer_fn writer, void *ctx)
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

void debug_console_register_dumps(void)
{
    static const debug_console_callbacks_t s_cbs = {
        .dump_dag      = dump_dag,
        .dump_wallet   = dump_wallet,
        .dump_currency = dump_currency,
        .dump_time     = dump_time,
    };
    esp_err_t err = debug_console_init(&s_cbs);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Console de debug : init echouee (err=0x%x)", (int)err);
    } else {
        ESP_LOGI(TAG, "Console de debug active "
                      "(commandes: help, dump_dag, dump_wallet, "
                      "dump_currency, dump_time, dump_all)");
    }
}

#endif /* CONFIG_MESHPAY_DEBUG_CONSOLE — [F-DC-004] garde source-level */
