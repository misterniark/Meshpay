/**
 * @file debug_console.c
 * @brief Implementation de la console serie de debug Mesh Pay.
 *
 * Tout le fichier est protege par CONFIG_MESHPAY_DEBUG_CONSOLE :
 * quand le flag est a 'n' (mode production), l'objet compile ne
 * contient qu'un fichier vide — le linker ne reserve aucune place
 * en flash ni en RAM. Le header se replie alors sur des stubs
 * static inline (cf. debug_console.h).
 *
 * Architecture interne :
 *
 *   +-----------------+
 *   | debug_task      |    (prio 2, stack 4 Ko)
 *   |  fgets(stdin)   |
 *   |    |            |
 *   |    v            |
 *   |  parse(line)    |--> debug_cmd_t (testable en isolation)
 *   |    |            |
 *   |    v            |
 *   |  dispatch       |--> appelle cbs.dump_dag(writer, &ctx)
 *   |                 |        cbs.dump_wallet(writer, &ctx) ...
 *   |                 |
 *   |  emet markers   |    <<<MESHPAY_DEBUG cmd BEGIN seq=N>>>
 *   |  + JSON lignes  |    {"...":...}
 *   |  + END          |    <<<MESHPAY_DEBUG cmd END>>>
 *   +-----------------+
 *
 * Le parser et le serializer hex sont exposes en API "amie" pour
 * que les tests Unity les exercent sans demarrer la tache UART.
 */

#include "sdkconfig.h"

#if CONFIG_MESHPAY_DEBUG_CONSOLE

#include "debug_console/debug_console.h"

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "esp_log.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* Console hardware : UART standard OU USB Serial JTAG selon le target.
 * Le choix est determine par la config IDF
 * (CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG sur ESP32-S3 Waveshare, UART0 sur
 * ESP32 CYD). On installe le driver correspondant et on route stdin
 * dessus pour que `fgets(..., stdin)` devienne reellement bloquant
 * et compatible ligne par ligne. */
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
#  include "driver/usb_serial_jtag.h"
#  include "esp_vfs_usb_serial_jtag.h"
#elif CONFIG_ESP_CONSOLE_UART
#  include "driver/uart.h"
/* `driver/uart_vfs.h` est le remplacant de `esp_vfs_dev.h` depuis
 * IDF v5.x. L'ancien header existe encore mais emet un deprecation
 * warning a la compilation. */
#  include "driver/uart_vfs.h"
#endif

static const char *TAG = "debug_console";

/* ================================================================
 * Etat interne
 * ================================================================ */

/** Etat global du composant. Initialise une seule fois par
 *  debug_console_init(). */
typedef struct {
    debug_console_callbacks_t cbs;        /**< Callbacks fournis par main.c */
    uint32_t                  seq;        /**< Compteur monotone des commandes */
    bool                      initialized;
} debug_state_t;

static debug_state_t s_state;

/* Pas de buffer ligne global : la DRAM est saturee (dette technique
 * « DRAM dram0_seg saturee »). Tous les buffers de formatage sont
 * alloues localement sur la stack de la tache `dbg_console` (4 Ko,
 * largement de marge pour un buffer ~512 octets). La tache etant
 * mono-thread, pas de souci de concurrence. */

/* ================================================================
 * Parser de commande (expose pour les tests)
 * ================================================================ */

/**
 * @brief Compare deux chaines sans tenir compte de la casse.
 *
 * Reimplementation locale de strcasecmp pour eviter d'avoir a tirer
 * `_GNU_SOURCE` ou `<strings.h>` (non standard sur ESP-IDF).
 */
static int debug_strcasecmp(const char *a, const char *b)
{
    while (*a && *b) {
        int ca = tolower((unsigned char)*a);
        int cb = tolower((unsigned char)*b);
        if (ca != cb) return ca - cb;
        a++; b++;
    }
    return tolower((unsigned char)*a) - tolower((unsigned char)*b);
}

debug_cmd_t debug_console_parse(const char *line)
{
    if (!line) return DEBUG_CMD_EMPTY;

    /* Trim de tete : sauter espaces, tabulations, CR. */
    while (*line && isspace((unsigned char)*line)) line++;
    if (*line == '\0') return DEBUG_CMD_EMPTY;

    /* Copier dans un buffer local pour pouvoir trimmer la queue
     * sans muter l'entree. On limite la taille (32 octets suffisent
     * pour la plus longue commande "dump_currency" = 13 + marge). */
    char buf[32];
    size_t i = 0;
    while (line[i] && !isspace((unsigned char)line[i]) && i < sizeof(buf) - 1) {
        buf[i] = (char)tolower((unsigned char)line[i]);
        i++;
    }
    buf[i] = '\0';

    /* Dispatch sur le premier token uniquement — pas d'arguments
     * acceptes dans cette premiere version. */
    if (debug_strcasecmp(buf, "help") == 0)          return DEBUG_CMD_HELP;
    if (debug_strcasecmp(buf, "dump_dag") == 0)      return DEBUG_CMD_DUMP_DAG;
    if (debug_strcasecmp(buf, "dump_wallet") == 0)   return DEBUG_CMD_DUMP_WALLET;
    if (debug_strcasecmp(buf, "dump_currency") == 0) return DEBUG_CMD_DUMP_CURRENCY;
    if (debug_strcasecmp(buf, "dump_time") == 0)     return DEBUG_CMD_DUMP_TIME;
    if (debug_strcasecmp(buf, "dump_all") == 0)      return DEBUG_CMD_DUMP_ALL;

    return DEBUG_CMD_UNKNOWN;
}

/* ================================================================
 * Serializer hex (expose pour les tests)
 * ================================================================ */

int debug_console_hex_encode(const uint8_t *src, size_t src_len,
                             char *dst, size_t dst_size)
{
    if (!src || !dst) return -1;
    /* Il faut src_len*2 caracteres + 1 pour le '\0' final. */
    if (dst_size < src_len * 2 + 1) return -1;

    static const char hex_chars[] = "0123456789abcdef";
    for (size_t i = 0; i < src_len; i++) {
        dst[i * 2]     = hex_chars[(src[i] >> 4) & 0x0F];
        dst[i * 2 + 1] = hex_chars[src[i] & 0x0F];
    }
    dst[src_len * 2] = '\0';
    return (int)(src_len * 2);
}

/* ================================================================
 * Writer de sortie : ecrit une ligne sur stdout puis un '\n'
 * ================================================================ */

/**
 * @brief Ecrivain par defaut : printf + flush.
 *
 * Le ctx est inutilise sur la prod (la sortie va sur la console
 * ESP-IDF). Les tests unitaires peuvent passer un ctx pointant vers
 * un buffer en memoire pour capturer la sortie sans UART.
 */
static void default_writer(const char *line, void *ctx)
{
    (void)ctx;
    if (!line) return;
    /* printf+fflush garantit que la ligne sort en bloc, ce qui
     * minimise les risques d'interleaving avec un ESP_LOG en cours
     * d'ecriture (sans non plus l'eliminer — un ESP_LOG est aussi
     * coupe en plusieurs writes). Le moniteur Python est concu pour
     * retrouver les marqueurs meme si du bruit s'intercale. */
    puts(line);          /* puts ajoute un '\n' final */
    fflush(stdout);
}

/* ================================================================
 * Helpers d'encadrement (BEGIN/END/ERR)
 * ================================================================ */

/** Nom textuel de la commande (utilise dans les marqueurs). */
static const char *cmd_name(debug_cmd_t cmd)
{
    switch (cmd) {
        case DEBUG_CMD_HELP:          return "help";
        case DEBUG_CMD_DUMP_DAG:      return "dump_dag";
        case DEBUG_CMD_DUMP_WALLET:   return "dump_wallet";
        case DEBUG_CMD_DUMP_CURRENCY: return "dump_currency";
        case DEBUG_CMD_DUMP_TIME:     return "dump_time";
        case DEBUG_CMD_DUMP_ALL:      return "dump_all";
        case DEBUG_CMD_UNKNOWN:       return "unknown";
        case DEBUG_CMD_EMPTY:         return "empty";
        default:                      return "?";
    }
}

/* Les marqueurs et lignes d'erreur sont courts (< 80 octets en
 * pratique) ; un buffer local de 96 octets sur la stack du caller
 * suffit, sans toucher la DRAM. */
#define DBG_MARK_BUF_SIZE 96

static void write_begin(const char *name, uint32_t seq)
{
    char buf[DBG_MARK_BUF_SIZE];
    snprintf(buf, sizeof(buf),
             "<<<MESHPAY_DEBUG %s BEGIN seq=%lu>>>",
             name, (unsigned long)seq);
    default_writer(buf, NULL);
}

static void write_end(const char *name)
{
    char buf[DBG_MARK_BUF_SIZE];
    snprintf(buf, sizeof(buf),
             "<<<MESHPAY_DEBUG %s END>>>",
             name);
    default_writer(buf, NULL);
}

static void write_err(const char *name, uint32_t seq, const char *err_code)
{
    char buf[DBG_MARK_BUF_SIZE];
    snprintf(buf, sizeof(buf),
             "<<<MESHPAY_DEBUG %s ERR seq=%lu>>>",
             name, (unsigned long)seq);
    default_writer(buf, NULL);
    snprintf(buf, sizeof(buf),
             "{\"err\":\"%s\"}", err_code);
    default_writer(buf, NULL);
    write_end(name);
}

/* ================================================================
 * Handlers de commande
 * ================================================================ */

/**
 * @brief Execute un callback de dump encadre par BEGIN/END.
 *
 * Si le callback est NULL (main.c ne l'a pas fourni), emet une
 * trame ERR `{"err":"not_implemented"}`. Si la commande est
 * inconnue, idem avec `{"err":"unknown_command"}`.
 */
static void dispatch_dump(debug_cmd_t cmd, debug_console_dump_fn cb)
{
    s_state.seq++;
    const char *name = cmd_name(cmd);

    if (cb == NULL) {
        write_err(name, s_state.seq, "not_implemented");
        return;
    }

    write_begin(name, s_state.seq);
    /* Le writer par defaut sort sur stdout. Le ctx est NULL — il
     * n'est utile qu'en test pour rediriger vers un buffer. */
    cb(default_writer, NULL);
    write_end(name);
}

/** Affiche la liste des commandes (commande `help`). */
static void emit_help(void)
{
    s_state.seq++;
    write_begin("help", s_state.seq);
    default_writer("{\"cmd\":\"help\",\"desc\":\"List commands\"}", NULL);
    default_writer("{\"cmd\":\"dump_dag\",\"desc\":\"Dump DAG transactions\"}", NULL);
    default_writer("{\"cmd\":\"dump_wallet\",\"desc\":\"Dump wallet state and locks\"}", NULL);
    default_writer("{\"cmd\":\"dump_currency\",\"desc\":\"Dump currency config\"}", NULL);
    default_writer("{\"cmd\":\"dump_time\",\"desc\":\"Dump time manager state\"}", NULL);
    default_writer("{\"cmd\":\"dump_all\",\"desc\":\"Run all four dumps\"}", NULL);
    write_end("help");
}

/** Execute la commande, dispatche sur les callbacks fournis. */
static void execute(debug_cmd_t cmd)
{
    switch (cmd) {
        case DEBUG_CMD_EMPTY:
            /* Ligne vide : silence (un moniteur peut envoyer des '\n'
             * pour ping). */
            break;

        case DEBUG_CMD_HELP:
            emit_help();
            break;

        case DEBUG_CMD_DUMP_DAG:
            dispatch_dump(cmd, s_state.cbs.dump_dag);
            break;

        case DEBUG_CMD_DUMP_WALLET:
            dispatch_dump(cmd, s_state.cbs.dump_wallet);
            break;

        case DEBUG_CMD_DUMP_CURRENCY:
            dispatch_dump(cmd, s_state.cbs.dump_currency);
            break;

        case DEBUG_CMD_DUMP_TIME:
            dispatch_dump(cmd, s_state.cbs.dump_time);
            break;

        case DEBUG_CMD_DUMP_ALL:
            dispatch_dump(DEBUG_CMD_DUMP_DAG,      s_state.cbs.dump_dag);
            dispatch_dump(DEBUG_CMD_DUMP_WALLET,   s_state.cbs.dump_wallet);
            dispatch_dump(DEBUG_CMD_DUMP_CURRENCY, s_state.cbs.dump_currency);
            dispatch_dump(DEBUG_CMD_DUMP_TIME,     s_state.cbs.dump_time);
            break;

        case DEBUG_CMD_UNKNOWN:
        default:
            s_state.seq++;
            write_err("unknown", s_state.seq, "unknown_command");
            break;
    }
}

/* ================================================================
 * Initialisation de la console hardware (UART ou USB Serial JTAG)
 * ================================================================ */

/**
 * @brief Installe le driver console et route stdin dessus.
 *
 * Sans ca, `fgets(stdin)` retourne NULL immediatement car aucun
 * mecanisme de blocage n'est en place — l'IO console par defaut
 * d'ESP-IDF est uniquement sortante.
 */
static esp_err_t console_io_init(void)
{
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
    /* ESP32-S3 Waveshare : console native via USB. */
    usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    esp_err_t err = usb_serial_jtag_driver_install(&cfg);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        /* INVALID_STATE = deja installe (par ex. par esp_console
         * dans un autre composant) — on accepte. */
        return err;
    }
    esp_vfs_usb_serial_jtag_use_driver();
    return ESP_OK;

#elif CONFIG_ESP_CONSOLE_UART
    /* ESP32 CYD : UART0 par USB-TTL externe. */
    const uart_port_t port = CONFIG_ESP_CONSOLE_UART_NUM;
    /* Buffer RX 256 octets : largement suffisant pour des commandes
     * courtes (< 32 caracteres) ; pas de buffer TX (on ecrit en
     * synchronous via printf qui passe par le ROM driver). */
    esp_err_t err = uart_driver_install(port, 256, 0, 0, NULL, 0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    /* API renommee dans IDF v5.x (cf. include ci-dessus). */
    uart_vfs_dev_use_driver(port);
    return ESP_OK;

#else
    /* Console ni UART ni USB JTAG (cas exotique : pas d'IO du tout).
     * On laisse passer : la tache va boucler sur fgets()=NULL avec
     * un petit delay pour ne pas tourner a vide. */
    return ESP_OK;
#endif
}

/* ================================================================
 * Tache FreeRTOS : boucle de lecture de commandes
 * ================================================================ */

static void debug_console_task(void *arg)
{
    (void)arg;

    /* Note : on initialise l'IO depuis la tache plutot que depuis
     * l'init pour ne pas bloquer app_main si un autre composant a
     * deja revendique le driver UART. */
    esp_err_t err = console_io_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Init IO console echouee (err=0x%x), passage en mode "
                      "polling sans blocage", (int)err);
    }

    /* Buffer ligne local a la stack de la tache (pas `static` : on
     * ne consomme pas de DRAM). Les commandes valides font moins de
     * 16 caracteres ; 64 octets laissent de la marge pour de futurs
     * arguments sans gonfler la stack. */
    char input_buf[64];

    ESP_LOGI(TAG, "Console de debug demarree. Commandes : help, dump_dag, "
                  "dump_wallet, dump_currency, dump_time, dump_all.");

    for (;;) {
        /* fgets renvoie NULL en cas d'erreur ou si stdin n'est pas
         * route. On evite la boucle serree en attendant une fraction
         * de seconde dans ce cas. */
        if (fgets(input_buf, sizeof(input_buf), stdin) == NULL) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        debug_cmd_t cmd = debug_console_parse(input_buf);
        execute(cmd);
    }
}

/* ================================================================
 * API publique
 * ================================================================ */

esp_err_t debug_console_init(const debug_console_callbacks_t *cbs)
{
    if (s_state.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    /* Copie locale des callbacks : main.c peut fournir une struct
     * sur la stack, on ne s'y fie pas durablement. cbs == NULL est
     * accepte (commande `help` reste utilisable, les dumps repondent
     * `not_implemented`). */
    memset(&s_state, 0, sizeof(s_state));
    if (cbs != NULL) {
        s_state.cbs = *cbs;
    }
    s_state.initialized = true;

    BaseType_t ok = xTaskCreate(
        debug_console_task,
        "dbg_console",
        CONFIG_MESHPAY_DEBUG_CONSOLE_TASK_STACK,
        NULL,
        CONFIG_MESHPAY_DEBUG_CONSOLE_TASK_PRIO,
        NULL);

    if (ok != pdPASS) {
        s_state.initialized = false;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

#endif /* CONFIG_MESHPAY_DEBUG_CONSOLE */
