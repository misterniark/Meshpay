/**
 * @file hal_lora_wio_e5.c
 * @brief Driver LoRa pour le module Grove Wio-E5 via UART/AT commands.
 *
 * Le Wio-E5 embarque un STM32WLE5 avec stack LoRa. La communication
 * se fait via commandes AT sur UART a 9600 bauds (defaut usine).
 *
 * Protocole AT utilise (mode TEST pour P2P) :
 * - AT                              -> verification de presence
 * - AT+MODE=TEST                    -> passage en mode test P2P
 * - AT+TEST=RFCFG,freq,SF,BW,...    -> configuration radio
 * - AT+TEST=TXLRPKT,"hex"           -> envoi d'un paquet LoRa
 * - AT+TEST=RXLRPKT                 -> passage en ecoute continue
 * - AT+LOWPOWER                     -> mise en veille
 *
 * Architecture :
 * - Les commandes AT sont envoyees de maniere synchrone (send_at_cmd)
 * - La reception est geree par une tache FreeRTOS dediee (rx_task)
 *   qui lit l'UART en continu et detecte les paquets recus
 * - Les paquets recus sont identifies par le prefixe "+TEST: RX "
 *   suivi des donnees en hexadecimal
 */

#include "hal_lora_wio_e5.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "hal_lora_wio_e5";

/* ================================================================
 * Constantes
 * ================================================================ */

/** Vitesse UART du Wio-E5 (defaut usine) */
#define WIO_UART_BAUD       9600

/** Taille du buffer UART en octets */
#define WIO_UART_BUF_SIZE   1024

/** Taille du buffer de reponse AT */
#define WIO_AT_RESP_SIZE    512

/** Timeout par defaut pour une reponse AT (ms) */
#define WIO_AT_TIMEOUT_MS   3000

/** Timeout long pour les commandes de configuration (ms) */
#define WIO_AT_LONG_TIMEOUT_MS  5000

/** Delimiteur de fin de reponse AT */
#define WIO_AT_CRLF         "\r\n"

/** Stack de la tache de reception */
#define WIO_RX_TASK_STACK   3072

/** Priorite de la tache de reception */
#define WIO_RX_TASK_PRIO    4

/** Prefixe d'un paquet recu dans la sortie AT */
#define WIO_RX_PREFIX       "+TEST: RX \""

/** Prefixe RSSI dans la sortie AT */
#define WIO_RSSI_PREFIX     "+TEST: RSSI"

/* ================================================================
 * Contexte interne
 * ================================================================ */

/** Contexte interne du driver Wio-E5 */
typedef struct {
    int              uart_num;       /**< Port UART utilise */
    int              tx_pin;         /**< GPIO TX */
    int              rx_pin;         /**< GPIO RX */
    bool             initialized;    /**< true apres init reussi */
    bool             rx_running;     /**< true si la tache RX tourne */
    hal_lora_rx_cb_t rx_cb;          /**< Callback de reception */
    void            *rx_user_ctx;    /**< Contexte utilisateur du callback */
    SemaphoreHandle_t uart_mutex;    /**< Mutex pour l'acces UART serie */
    SemaphoreHandle_t rx_stop_sem;   /**< [F-HW-003] Synchro fin de tache RX */
    TaskHandle_t     rx_task_handle; /**< Handle de la tache de reception */
    int16_t          last_rssi;      /**< Dernier RSSI recu */
} wio_e5_ctx_t;

static wio_e5_ctx_t s_wio_ctx;

/* ================================================================
 * Fonctions utilitaires bas niveau
 * ================================================================ */

/**
 * @brief Envoie une commande AT sur l'UART et attend la reponse.
 *
 * Envoie la commande suivie de \r\n, puis attend une reponse
 * contenant le pattern attendu ou un timeout.
 *
 * @param ctx         Contexte du driver
 * @param cmd         Commande AT (sans le \r\n)
 * @param resp        Buffer de reponse (peut etre NULL si on ignore la reponse)
 * @param resp_size   Taille du buffer de reponse
 * @param expect      Pattern attendu dans la reponse (ex: "+AT: OK")
 * @param timeout_ms  Timeout en millisecondes
 * @return HAL_OK si le pattern est trouve, HAL_ERR_TIMEOUT sinon
 */
static hal_err_t send_at_cmd(wio_e5_ctx_t *ctx, const char *cmd,
                             char *resp, size_t resp_size,
                             const char *expect, uint32_t timeout_ms)
{
    /* Vider le buffer de reception UART avant d'envoyer */
    uart_flush_input(ctx->uart_num);

    /* Envoyer la commande + \r\n */
    uart_write_bytes(ctx->uart_num, cmd, strlen(cmd));
    uart_write_bytes(ctx->uart_num, WIO_AT_CRLF, 2);

    ESP_LOGD(TAG, "TX: %s", cmd);

    /* Buffer local si l'appelant n'en fournit pas */
    char local_buf[WIO_AT_RESP_SIZE];
    char *buf = resp ? resp : local_buf;
    size_t buf_size = resp ? resp_size : sizeof(local_buf);

    /* Lire la reponse avec timeout */
    size_t total_read = 0;
    uint64_t start = xTaskGetTickCount();
    uint32_t timeout_ticks = pdMS_TO_TICKS(timeout_ms);

    while ((xTaskGetTickCount() - start) < timeout_ticks) {
        int avail = 0;
        uart_get_buffered_data_len(ctx->uart_num, (size_t *)&avail);

        if (avail > 0) {
            size_t to_read = avail;
            if (total_read + to_read >= buf_size - 1) {
                to_read = buf_size - 1 - total_read;
            }
            if (to_read > 0) {
                int n = uart_read_bytes(ctx->uart_num, (uint8_t *)buf + total_read,
                                        to_read, pdMS_TO_TICKS(100));
                if (n > 0) {
                    total_read += n;
                    buf[total_read] = '\0';

                    ESP_LOGD(TAG, "RX partial (%d): %s", (int)total_read, buf);

                    /* Verifier si la reponse attendue est presente */
                    if (expect && strstr(buf, expect)) {
                        ESP_LOGD(TAG, "RX OK: pattern '%s' trouve", expect);
                        return HAL_OK;
                    }

                    /*
                     * [F-HW-004] Détection d'erreur restreinte au
                     * préfixe AT strict (`+AT: ERROR`). L'ancienne
                     * détection générique `strstr("ERROR")` provoquait
                     * de faux positifs si un autre fragment contenait
                     * le mot.
                     */
                    if (strstr(buf, "+AT: ERROR")) {
                        ESP_LOGW(TAG, "Erreur AT: %s", buf);
                        return HAL_FAIL;
                    }
                }
            }
        } else {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }

    buf[total_read] = '\0';
    ESP_LOGW(TAG, "Timeout AT (%"PRIu32"ms), recu: '%s'", timeout_ms, buf);
    return HAL_ERR_TIMEOUT;
}

/**
 * @brief Convertit un buffer binaire en chaine hexadecimale.
 *
 * @param data    Donnees binaires
 * @param len     Taille des donnees
 * @param hex_out Buffer de sortie (doit etre >= len*2 + 1)
 */
static void bin_to_hex(const uint8_t *data, size_t len, char *hex_out)
{
    static const char hex_chars[] = "0123456789ABCDEF";
    for (size_t i = 0; i < len; i++) {
        hex_out[i * 2]     = hex_chars[(data[i] >> 4) & 0x0F];
        hex_out[i * 2 + 1] = hex_chars[data[i] & 0x0F];
    }
    hex_out[len * 2] = '\0';
}

/**
 * @brief Convertit un caractere hexadecimal en valeur 0-15.
 *
 * @return Valeur 0-15, ou -1 si caractere invalide
 */
static int hex_char_to_val(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

/**
 * @brief Convertit une chaine hexadecimale en buffer binaire.
 *
 * @param hex     Chaine hexadecimale (longueur paire)
 * @param hex_len Longueur de la chaine hex
 * @param bin_out Buffer de sortie binaire
 * @param max_out Taille max du buffer de sortie
 * @return Nombre d'octets ecrits, ou -1 en cas d'erreur
 */
static int hex_to_bin(const char *hex, size_t hex_len,
                      uint8_t *bin_out, size_t max_out)
{
    if (hex_len % 2 != 0) return -1;

    size_t bin_len = hex_len / 2;
    if (bin_len > max_out) return -1;

    for (size_t i = 0; i < bin_len; i++) {
        int hi = hex_char_to_val(hex[i * 2]);
        int lo = hex_char_to_val(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) return -1;
        bin_out[i] = (uint8_t)((hi << 4) | lo);
    }
    return (int)bin_len;
}

/**
 * @brief Parse un paquet recu depuis la sortie AT du Wio-E5.
 *
 * Format attendu : +TEST: RX "AABBCCDD..."
 * Extrait les donnees hexadecimales et les convertit en binaire.
 *
 * @param line     Ligne de reponse AT
 * @param data_out Buffer de sortie binaire
 * @param max_out  Taille max du buffer
 * @return Nombre d'octets du paquet, ou -1 si pas un paquet RX
 */
static int parse_rx_packet(const char *line, uint8_t *data_out, size_t max_out)
{
    /* Chercher le prefixe RX */
    const char *start = strstr(line, WIO_RX_PREFIX);
    if (!start) return -1;

    /* Avancer apres le prefixe (inclut le guillemet ouvrant) */
    start += strlen(WIO_RX_PREFIX);

    /* Chercher le guillemet fermant */
    const char *end = strchr(start, '"');
    if (!end) return -1;

    size_t hex_len = end - start;
    return hex_to_bin(start, hex_len, data_out, max_out);
}

/**
 * @brief Parse le RSSI depuis la sortie AT.
 *
 * Format : +TEST: RSSI -xx, SNR yy
 *
 * @param line Ligne contenant le RSSI
 * @return RSSI en dBm, ou 0 si non trouve
 */
static int16_t parse_rssi(const char *line)
{
    const char *p = strstr(line, WIO_RSSI_PREFIX);
    if (!p) return 0;

    /* Avancer apres "RSSI " */
    p += strlen(WIO_RSSI_PREFIX);
    while (*p == ' ' || *p == ':') p++;

    return (int16_t)atoi(p);
}

/* ================================================================
 * Tache de reception (FreeRTOS)
 * ================================================================ */

/**
 * @brief Tache de reception LoRa.
 *
 * Lit l'UART en continu et detecte les paquets recus.
 * Le Wio-E5 en mode RXLRPKT envoie les paquets ligne par ligne :
 *   +TEST: RX "AABBCCDD..."
 *   +TEST: RSSI -45, SNR 10
 *
 * La tache accumule les caracteres ligne par ligne et analyse
 * chaque ligne complete.
 */
static void wio_rx_task(void *param)
{
    wio_e5_ctx_t *ctx = (wio_e5_ctx_t *)param;

    /* Buffer pour accumuler une ligne de reponse AT */
    char line_buf[WIO_AT_RESP_SIZE];
    size_t line_pos = 0;

    /* Buffer pour le paquet binaire decode */
    uint8_t pkt_buf[HAL_LORA_MAX_PACKET_SIZE];
    int pkt_len = -1;

    ESP_LOGI(TAG, "Tache RX demarree");

    while (ctx->rx_running) {
        uint8_t byte;
        int n = uart_read_bytes(ctx->uart_num, &byte, 1, pdMS_TO_TICKS(100));
        if (n <= 0) continue;

        /* Accumuler dans le buffer de ligne */
        if (byte == '\n') {
            line_buf[line_pos] = '\0';

            if (line_pos > 0) {
                ESP_LOGD(TAG, "RX line: %s", line_buf);

                /* Essayer de parser un paquet RX */
                int len = parse_rx_packet(line_buf, pkt_buf, sizeof(pkt_buf));
                if (len > 0) {
                    pkt_len = len;
                    /* Le RSSI arrive sur la ligne suivante */
                }

                /* Essayer de parser le RSSI */
                int16_t rssi = parse_rssi(line_buf);
                if (rssi != 0) {
                    ctx->last_rssi = rssi;
                }

                /*
                 * Si on a un paquet en attente et qu'on vient de recevoir
                 * le RSSI (ou une nouvelle ligne), livrer le paquet.
                 */
                if (pkt_len > 0 && (rssi != 0 || strstr(line_buf, "+TEST:"))) {
                    if (ctx->rx_cb) {
                        ctx->rx_cb(pkt_buf, (size_t)pkt_len,
                                   ctx->last_rssi, ctx->rx_user_ctx);
                    }
                    pkt_len = -1;
                }
            }
            line_pos = 0;
        } else if (byte != '\r') {
            if (line_pos < sizeof(line_buf) - 1) {
                line_buf[line_pos++] = (char)byte;
            }
        }
    }

    ESP_LOGI(TAG, "Tache RX arretee");
    /*
     * [F-HW-003] Signaler la terminaison effective avant l'auto-delete
     * pour que `wio_sleep` puisse attendre proprement (sémaphore au
     * lieu de timing aveugle).
     */
    if (ctx->rx_stop_sem) {
        xSemaphoreGive(ctx->rx_stop_sem);
    }
    vTaskDelete(NULL);
}

/* ================================================================
 * Implementation des fonctions de la vtable
 * ================================================================ */

/**
 * @brief Initialise le module Wio-E5.
 *
 * Sequence :
 * 1. Configurer l'UART ESP32
 * 2. Envoyer AT pour verifier la presence du module
 * 3. Passer en mode TEST (P2P)
 * 4. Configurer les parametres radio (frequence, SF, BW, etc.)
 */
static hal_err_t wio_init(const hal_lora_config_t *config, void *ctx_ptr)
{
    wio_e5_ctx_t *ctx = (wio_e5_ctx_t *)ctx_ptr;

    if (ctx->initialized) {
        return HAL_OK;
    }

    /* 1. Configuration UART ESP32 */
    uart_config_t uart_cfg = {
        .baud_rate  = WIO_UART_BAUD,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    esp_err_t err = uart_param_config(ctx->uart_num, &uart_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_param_config echoue: %d", err);
        return HAL_FAIL;
    }

    err = uart_set_pin(ctx->uart_num, ctx->tx_pin, ctx->rx_pin,
                       UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_set_pin echoue: %d", err);
        return HAL_FAIL;
    }

    err = uart_driver_install(ctx->uart_num, WIO_UART_BUF_SIZE,
                              WIO_UART_BUF_SIZE, 0, NULL, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_driver_install echoue: %d", err);
        return HAL_FAIL;
    }

    /* Creer le mutex pour l'acces UART */
    ctx->uart_mutex = xSemaphoreCreateMutex();
    if (!ctx->uart_mutex) {
        ESP_LOGE(TAG, "Creation mutex echouee");
        uart_driver_delete(ctx->uart_num);
        return HAL_FAIL;
    }

    /* Petit delai pour laisser le module demarrer */
    vTaskDelay(pdMS_TO_TICKS(200));

    /* 2. Verifier la presence du module avec AT */
    hal_err_t ret = send_at_cmd(ctx, "AT", NULL, 0, "+AT: OK",
                                WIO_AT_TIMEOUT_MS);
    if (ret != HAL_OK) {
        ESP_LOGE(TAG, "Module Wio-E5 non detecte (pas de reponse AT)");
        uart_driver_delete(ctx->uart_num);
        vSemaphoreDelete(ctx->uart_mutex);
        return HAL_FAIL;
    }
    ESP_LOGI(TAG, "Module Wio-E5 detecte");

    /* 3. Passer en mode TEST (P2P) */
    ret = send_at_cmd(ctx, "AT+MODE=TEST", NULL, 0, "+MODE: TEST",
                      WIO_AT_LONG_TIMEOUT_MS);
    if (ret != HAL_OK) {
        ESP_LOGW(TAG, "AT+MODE=TEST echoue (peut-etre deja en mode TEST)");
        /* On continue quand meme — le module est peut-etre deja en mode TEST */
    }

    /* 4. Configurer les parametres radio */
    if (config) {
        /*
         * Format AT+TEST=RFCFG : freq, SF, BW, TX_PR, RX_PR, POWER, CR
         *
         * BW mapping : 0=125, 1=250, 2=500 -> valeur en kHz pour la commande AT
         * CR mapping : 1=4/5, 2=4/6, 3=4/7, 4=4/8 -> valeur 1-4
         */
        uint32_t bw_khz;
        switch (config->bandwidth) {
            case 0:  bw_khz = 125; break;
            case 1:  bw_khz = 250; break;
            case 2:  bw_khz = 500; break;
            default: bw_khz = 125; break;
        }

        uint8_t cr = config->coding_rate;
        if (cr < 1 || cr > 4) cr = 1;

        char at_cmd[128];
        snprintf(at_cmd, sizeof(at_cmd),
                 "AT+TEST=RFCFG,%"PRIu32",%u,%"PRIu32",8,8,%d,%u",
                 config->frequency_hz,
                 config->spreading_factor,
                 bw_khz,
                 config->tx_power_dbm,
                 cr);

        ret = send_at_cmd(ctx, at_cmd, NULL, 0, "+TEST: RFCFG",
                          WIO_AT_LONG_TIMEOUT_MS);
        if (ret != HAL_OK) {
            ESP_LOGE(TAG, "Configuration radio echouee");
            uart_driver_delete(ctx->uart_num);
            vSemaphoreDelete(ctx->uart_mutex);
            return HAL_FAIL;
        }

        ESP_LOGI(TAG, "Radio configuree: %"PRIu32" Hz, SF%u, BW%"PRIu32", %d dBm",
                 config->frequency_hz, config->spreading_factor,
                 bw_khz, config->tx_power_dbm);
    }

    ctx->initialized = true;
    ctx->rx_running = false;
    ctx->last_rssi = 0;

    ESP_LOGI(TAG, "Wio-E5 initialise (UART%d)", ctx->uart_num);
    return HAL_OK;
}

/**
 * @brief Envoie un paquet LoRa (bloquant).
 *
 * Convertit les donnees en hexadecimal et envoie via AT+TEST=TXLRPKT.
 * Le module repasse en RX automatiquement apres l'envoi (si start_rx
 * a ete appele avant).
 */
static hal_err_t wio_send(const uint8_t *data, size_t len, void *ctx_ptr)
{
    wio_e5_ctx_t *ctx = (wio_e5_ctx_t *)ctx_ptr;

    if (!ctx->initialized) {
        ESP_LOGE(TAG, "send: module non initialise");
        return HAL_FAIL;
    }

    if (data == NULL || len == 0) {
        return HAL_ERR_INVALID;
    }

    if (len > HAL_LORA_MAX_PACKET_SIZE) {
        ESP_LOGE(TAG, "send: paquet trop grand (%d > %d)",
                 (int)len, HAL_LORA_MAX_PACKET_SIZE);
        return HAL_ERR_INVALID;
    }

    /* Prendre le mutex pour l'acces UART */
    if (xSemaphoreTake(ctx->uart_mutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
        ESP_LOGE(TAG, "send: timeout mutex UART");
        return HAL_ERR_BUSY;
    }

    /* Convertir les donnees en hexadecimal */
    char hex_buf[HAL_LORA_MAX_PACKET_SIZE * 2 + 1];
    bin_to_hex(data, len, hex_buf);

    /* Construire la commande AT */
    char at_cmd[HAL_LORA_MAX_PACKET_SIZE * 2 + 32];
    snprintf(at_cmd, sizeof(at_cmd), "AT+TEST=TXLRPKT,\"%s\"", hex_buf);

    /* Envoyer et attendre la confirmation */
    hal_err_t ret = send_at_cmd(ctx, at_cmd, NULL, 0, "+TEST: TXLRPKT",
                                WIO_AT_LONG_TIMEOUT_MS);

    xSemaphoreGive(ctx->uart_mutex);

    if (ret == HAL_OK) {
        ESP_LOGD(TAG, "Paquet envoye (%d octets)", (int)len);

        /*
         * Apres un envoi, le module sort du mode RX.
         * Si la tache RX tourne, on relance le mode RX.
         */
        if (ctx->rx_running) {
            if (xSemaphoreTake(ctx->uart_mutex, pdMS_TO_TICKS(2000)) == pdTRUE) {
                send_at_cmd(ctx, "AT+TEST=RXLRPKT", NULL, 0, "+TEST: RXLRPKT",
                            WIO_AT_TIMEOUT_MS);
                xSemaphoreGive(ctx->uart_mutex);
            }
        }
    } else {
        ESP_LOGE(TAG, "Envoi echoue");
    }

    return ret;
}

/**
 * @brief Enregistre le callback de reception.
 */
static hal_err_t wio_set_rx_callback(hal_lora_rx_cb_t cb, void *user_ctx,
                                     void *ctx_ptr)
{
    wio_e5_ctx_t *ctx = (wio_e5_ctx_t *)ctx_ptr;
    ctx->rx_cb = cb;
    ctx->rx_user_ctx = user_ctx;
    return HAL_OK;
}

/**
 * @brief Active le mode reception continue.
 *
 * Lance la tache FreeRTOS de lecture UART et envoie la commande
 * AT+TEST=RXLRPKT pour passer le module en ecoute.
 */
static hal_err_t wio_start_rx(void *ctx_ptr)
{
    wio_e5_ctx_t *ctx = (wio_e5_ctx_t *)ctx_ptr;

    if (!ctx->initialized) {
        ESP_LOGE(TAG, "start_rx: module non initialise");
        return HAL_FAIL;
    }

    if (!ctx->rx_cb) {
        ESP_LOGE(TAG, "start_rx: aucun callback enregistre");
        return HAL_ERR_INVALID;
    }

    if (ctx->rx_running) {
        ESP_LOGD(TAG, "start_rx: deja en cours");
        return HAL_OK;
    }

    /*
     * [F-HW-011] Envoyer AT+TEST=RXLRPKT AVANT de démarrer la tâche RX.
     * Sans cet ordre, la tâche commençait à lire l'UART avant que le
     * module soit en mode RX et capturait la réponse AT au lieu de
     * `send_at_cmd`.
     */
    if (xSemaphoreTake(ctx->uart_mutex, pdMS_TO_TICKS(2000)) == pdTRUE) {
        send_at_cmd(ctx, "AT+TEST=RXLRPKT", NULL, 0, "+TEST: RXLRPKT",
                    WIO_AT_TIMEOUT_MS);
        xSemaphoreGive(ctx->uart_mutex);
    }

    /* [F-HW-003] Sémaphore de fin alloué une seule fois (binaire). */
    if (ctx->rx_stop_sem == NULL) {
        ctx->rx_stop_sem = xSemaphoreCreateBinary();
        if (!ctx->rx_stop_sem) {
            ESP_LOGE(TAG, "start_rx: rx_stop_sem non alloue");
            return HAL_ERR_NO_MEM;
        }
    }

    /* Lancer la tache de reception */
    ctx->rx_running = true;
    BaseType_t ret = xTaskCreate(wio_rx_task, "wio_rx", WIO_RX_TASK_STACK,
                                 ctx, WIO_RX_TASK_PRIO, &ctx->rx_task_handle);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Creation tache RX echouee");
        ctx->rx_running = false;
        return HAL_FAIL;
    }

    ESP_LOGI(TAG, "Mode reception active");
    return HAL_OK;
}

/**
 * @brief Met le module en veille basse consommation.
 *
 * Arrete la tache de reception si elle tourne, puis envoie
 * la commande AT+LOWPOWER.
 */
static hal_err_t wio_sleep(void *ctx_ptr)
{
    wio_e5_ctx_t *ctx = (wio_e5_ctx_t *)ctx_ptr;

    if (!ctx->initialized) {
        return HAL_OK;  /* Rien a faire */
    }

    /*
     * [F-HW-003] Arrêt synchronisé de la tâche RX via sémaphore.
     * Avant : `vTaskDelay(200 ms)` aveugle, insuffisant si la tâche
     * boucle sur `uart_read_bytes(timeout=100ms)`.
     */
    if (ctx->rx_running) {
        ctx->rx_running = false;
        if (ctx->rx_stop_sem) {
            if (xSemaphoreTake(ctx->rx_stop_sem, pdMS_TO_TICKS(1000)) != pdTRUE) {
                ESP_LOGW(TAG, "sleep : tache RX n'a pas signale sa terminaison");
            }
        } else {
            vTaskDelay(pdMS_TO_TICKS(400));
        }
        ctx->rx_task_handle = NULL;
    }

    /* Envoyer la commande de mise en veille */
    if (xSemaphoreTake(ctx->uart_mutex, pdMS_TO_TICKS(2000)) == pdTRUE) {
        send_at_cmd(ctx, "AT+LOWPOWER", NULL, 0, "+LOWPOWER",
                    WIO_AT_TIMEOUT_MS);
        xSemaphoreGive(ctx->uart_mutex);
    }

    ESP_LOGI(TAG, "Module en veille");
    return HAL_OK;
}

/* ================================================================
 * Factory
 * ================================================================ */

hal_err_t hal_lora_wio_e5_create(hal_lora_t *lora,
                                 int uart_num, int tx_pin, int rx_pin)
{
    if (!lora) {
        return HAL_ERR_INVALID;
    }

    /* Initialiser le contexte */
    memset(&s_wio_ctx, 0, sizeof(wio_e5_ctx_t));
    s_wio_ctx.uart_num    = uart_num;
    s_wio_ctx.tx_pin      = tx_pin;
    s_wio_ctx.rx_pin      = rx_pin;
    s_wio_ctx.initialized = false;
    s_wio_ctx.rx_running  = false;
    s_wio_ctx.rx_cb       = NULL;
    s_wio_ctx.rx_user_ctx = NULL;

    /* Remplir la vtable avec les fonctions reelles */
    lora->init            = wio_init;
    lora->send            = wio_send;
    lora->set_rx_callback = wio_set_rx_callback;
    lora->start_rx        = wio_start_rx;
    lora->sleep           = wio_sleep;
    lora->ctx             = &s_wio_ctx;

    ESP_LOGI(TAG, "Driver cree (UART%d, TX=%d, RX=%d)", uart_num, tx_pin, rx_pin);
    return HAL_OK;
}
