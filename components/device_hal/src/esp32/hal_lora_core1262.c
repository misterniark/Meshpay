/**
 * @file hal_lora_core1262.c
 * @brief Backend HAL LoRa pour le module Waveshare Core1262 (SX1262 SPI).
 *
 * Remplit la vtable hal_lora_t en pilotant le SX1262 via le pilote
 * Semtech vendore (sx126x/) et la glue plateforme (sx126x_hal.c).
 *
 * Architecture :
 * - init : bus + device SPI, GPIO, reset, TCXO (DIO3), calibration,
 *   config LoRa (frequence/PA/modulation), routage IRQ.
 * - send : bloquant. Bascule le switch RF en TX, ecrit le buffer,
 *   lance l'emission, attend TX_DONE en scrutant le registre IRQ.
 * - reception : la tache rx_task est reveillee par l'ISR du pin DIO1
 *   (routage IRQ : seul RX_DONE/TIMEOUT est route sur DIO1). Elle lit
 *   le paquet et appelle le callback applicatif.
 * - Un mutex serialise tous les acces radio entre send() et rx_task.
 *
 * Switch RF : le module Core1262 expose RXEN/TXEN ; on les pilote a la
 * main (RXEN=1/TXEN=0 en reception, l'inverse en emission).
 *
 * Reception continue : on utilise sx126x_set_rx_with_timeout_in_rtc_step()
 * avec SX126X_RX_CONTINUOUS (0x00FFFFFF) — la valeur 0 correspond a
 * SX126X_RX_SINGLE_MODE (reception unique puis retour standby).
 */

#include "hal_lora_core1262.h"
#include "sx126x_hal_context.h"
#include "core1262_params.h"
#include "sx126x.h"
#include "sx126x_hal.h"

#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include <string.h>

/* sx126x_write_buffer prend une taille uint8_t : la limite HAL doit tenir. */
_Static_assert(HAL_LORA_MAX_PACKET_SIZE <= 255,
               "HAL_LORA_MAX_PACKET_SIZE depasse la limite SX1262 (255)");

static const char *TAG = "hal_lora_c1262";

/* ================================================================
 * Constantes
 * ================================================================ */

/** Horloge SPI : 8 MHz (le SX1262 supporte 18 MHz ; marge pour fils volants). */
#define C1262_SPI_CLOCK_HZ   (8 * 1000 * 1000)

/** Tension de controle du TCXO via DIO3. A confirmer au smoke test. */
#define C1262_TCXO_VOLTAGE   SX126X_TCXO_CTRL_1_8V
/** Delai de stabilisation du TCXO, en pas RTC de 15.625 us (~5 ms). */
#define C1262_TCXO_TIMEOUT   320

/** Stack et priorite de la tache RX. */
#define C1262_RX_TASK_STACK  4096
#define C1262_RX_TASK_PRIO   5

/** Timeout d'attente de TX_DONE (ms). */
#define C1262_TX_TIMEOUT_MS  4000

/* ================================================================
 * Contexte interne
 * ================================================================ */

typedef struct {
    core1262_hw_t           hw;           /**< SPI + NSS/BUSY/RESET (passe au pilote Semtech) */
    int                     pin_dio1;     /**< GPIO IRQ */
    int                     pin_rxen;     /**< GPIO switch RF RX */
    int                     pin_txen;     /**< GPIO switch RF TX */
    int                     spi_host;     /**< Hote SPI */
    int                     pin_sck;
    int                     pin_mosi;
    int                     pin_miso;

    core1262_radio_params_t params;       /**< Parametres radio derives de la config */

    bool                    initialized;
    volatile bool           rx_running;
    hal_lora_rx_cb_t        rx_cb;
    void                   *rx_user_ctx;

    SemaphoreHandle_t       radio_mutex;  /**< Serialise les acces radio */
    SemaphoreHandle_t       dio1_sem;     /**< Donne par l'ISR DIO1 (RX_DONE) */
    TaskHandle_t            rx_task_handle;
} core1262_ctx_t;

static core1262_ctx_t s_ctx;

/* ================================================================
 * Helpers bas niveau
 * ================================================================ */

/** Bascule le switch RF en mode reception. */
static void rf_switch_rx(core1262_ctx_t *ctx)
{
    gpio_set_level(ctx->pin_txen, 0);
    gpio_set_level(ctx->pin_rxen, 1);
}

/** Bascule le switch RF en mode emission. */
static void rf_switch_tx(core1262_ctx_t *ctx)
{
    gpio_set_level(ctx->pin_rxen, 0);
    gpio_set_level(ctx->pin_txen, 1);
}

/** ISR du pin DIO1 : reveille la tache RX. */
static void IRAM_ATTR dio1_isr_handler(void *arg)
{
    core1262_ctx_t *ctx = (core1262_ctx_t *)arg;
    BaseType_t hp_task_woken = pdFALSE;
    xSemaphoreGiveFromISR(ctx->dio1_sem, &hp_task_woken);
    if (hp_task_woken == pdTRUE) {
        portYIELD_FROM_ISR();
    }
}

/**
 * Applique toute la config radio sur le SX1262 (hors taille de paquet).
 * Appele une fois a l'init. Retourne HAL_OK / HAL_ERR_IO.
 */
static hal_err_t apply_radio_config(core1262_ctx_t *ctx)
{
    const void *sx = &ctx->hw;
    sx126x_status_t st;

    /* Sortie de reset -> standby RC. */
    st = sx126x_set_standby(sx, SX126X_STANDBY_CFG_RC);
    if (st != SX126X_STATUS_OK) return HAL_ERR_IO;

    /* Le module Core1262 a un TCXO alimente par DIO3 : le declarer AVANT
     * la calibration, sinon la calibration PLL echoue. */
    st = sx126x_set_dio3_as_tcxo_ctrl(sx, C1262_TCXO_VOLTAGE, C1262_TCXO_TIMEOUT);
    if (st != SX126X_STATUS_OK) return HAL_ERR_IO;

    /* Calibration complete (RC, PLL, ADC, image). */
    st = sx126x_cal(sx, SX126X_CAL_ALL);
    if (st != SX126X_STATUS_OK) return HAL_ERR_IO;

    /* Le switch RF est pilote par RXEN/TXEN (GPIO), pas par DIO2 : on
     * desactive explicitement le controle DIO2. */
    st = sx126x_set_dio2_as_rf_sw_ctrl(sx, false);
    if (st != SX126X_STATUS_OK) return HAL_ERR_IO;

    /* Type de paquet : LoRa. */
    st = sx126x_set_pkt_type(sx, SX126X_PKT_TYPE_LORA);
    if (st != SX126X_STATUS_OK) return HAL_ERR_IO;

    /* Frequence porteuse. */
    st = sx126x_set_rf_freq(sx, ctx->params.freq_hz);
    if (st != SX126X_STATUS_OK) return HAL_ERR_IO;

    /* Configuration de l'amplificateur de puissance (SX1262, +14 dBm).
     * Valeurs issues de la table Semtech AN1200.x pour le SX1262. A
     * reverifier au smoke test si la portee est anormale. */
    const sx126x_pa_cfg_params_t pa_cfg = {
        .pa_duty_cycle = 0x02,
        .hp_max        = 0x02,
        .device_sel    = 0x00,  /* 0x00 = SX1262 */
        .pa_lut        = 0x01,
    };
    st = sx126x_set_pa_cfg(sx, &pa_cfg);
    if (st != SX126X_STATUS_OK) return HAL_ERR_IO;

    st = sx126x_set_tx_params(sx, ctx->params.power_dbm, SX126X_RAMP_200_US);
    if (st != SX126X_STATUS_OK) return HAL_ERR_IO;

    /* Parametres de modulation LoRa (SF/BW/CR/LDRO). */
    st = sx126x_set_lora_mod_params(sx, &ctx->params.mod);
    if (st != SX126X_STATUS_OK) return HAL_ERR_IO;

    /* Adresses de base des buffers TX/RX dans la FIFO du SX1262. */
    st = sx126x_set_buffer_base_address(sx, 0x00, 0x00);
    if (st != SX126X_STATUS_OK) return HAL_ERR_IO;

    /* Routage des IRQ : TX_DONE/RX_DONE/TIMEOUT/CRC_ERROR actives dans le
     * registre d'etat. Seuls RX_DONE et TIMEOUT sont routes sur DIO1 (le
     * SX1262 leve RX_DONE meme pour un paquet CRC invalide, ce qui permet
     * a la tache RX de detecter et rejeter les paquets corrompus). TX_DONE
     * reste lisible dans le registre de status (scrute par send()). */
    const uint16_t irq_mask  = SX126X_IRQ_TX_DONE | SX126X_IRQ_RX_DONE |
                               SX126X_IRQ_TIMEOUT | SX126X_IRQ_CRC_ERROR;
    const uint16_t dio1_mask = SX126X_IRQ_RX_DONE | SX126X_IRQ_TIMEOUT;
    st = sx126x_set_dio_irq_params(sx, irq_mask, dio1_mask, 0x0000, 0x0000);
    if (st != SX126X_STATUS_OK) return HAL_ERR_IO;

    return HAL_OK;
}

/* ================================================================
 * Tache de reception
 * ================================================================ */

static void c1262_rx_task(void *param)
{
    core1262_ctx_t *ctx = (core1262_ctx_t *)param;
    const void *sx = &ctx->hw;
    uint8_t pkt_buf[HAL_LORA_MAX_PACKET_SIZE];

    ESP_LOGI(TAG, "Tache RX demarree");

    while (ctx->rx_running) {
        /* Attendre une IRQ DIO1 (RX_DONE/TIMEOUT). Reveil periodique pour
         * pouvoir sortir proprement si rx_running passe a false. */
        if (xSemaphoreTake(ctx->dio1_sem, pdMS_TO_TICKS(500)) != pdTRUE) {
            continue;
        }

        if (xSemaphoreTake(ctx->radio_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
            ESP_LOGW(TAG, "RX : mutex radio indisponible");
            continue;
        }

        sx126x_irq_mask_t irq = 0;
        sx126x_get_irq_status(sx, &irq);
        sx126x_clear_irq_status(sx, irq);

        /* M2 : signaler un TIMEOUT inattendu en mode continu (ne devrait pas
         * arriver avec SX126X_RX_CONTINUOUS, utile pour le debug materiel). */
        if (irq & SX126X_IRQ_TIMEOUT) {
            ESP_LOGW(TAG, "RX : IRQ TIMEOUT inattendu en mode continu");
        }

        int     pkt_len = -1;
        int16_t rssi    = 0;

        if (irq & SX126X_IRQ_RX_DONE) {
            /* M3(b) : rejeter silencieusement les paquets avec CRC invalide. */
            if (irq & SX126X_IRQ_CRC_ERROR) {
                ESP_LOGW(TAG, "RX : paquet rejete (CRC invalide)");
            } else {
                sx126x_rx_buffer_status_t rxb = {0};
                if (sx126x_get_rx_buffer_status(sx, &rxb) == SX126X_STATUS_OK &&
                    rxb.pld_len_in_bytes > 0 &&
                    rxb.pld_len_in_bytes <= HAL_LORA_MAX_PACKET_SIZE) {

                    if (sx126x_read_buffer(sx, rxb.buffer_start_pointer, pkt_buf,
                                           rxb.pld_len_in_bytes) == SX126X_STATUS_OK) {
                        pkt_len = rxb.pld_len_in_bytes;

                        sx126x_pkt_status_lora_t pst = {0};
                        if (sx126x_get_lora_pkt_status(sx, &pst) == SX126X_STATUS_OK) {
                            rssi = pst.rssi_pkt_in_dbm;
                        }
                    }
                }
            }
        }

        /* C1 : si sleep() a demande l'arret pendant qu'on tenait le mutex,
         * ne PAS re-armer le radio : on sort de la boucle proprement.
         * Sans ce test, sleep() pourrait remettre le SX1262 en standby
         * puis cette tache le re-basculerait en reception (race). */
        if (!ctx->rx_running) {
            xSemaphoreGive(ctx->radio_mutex);
            break;
        }

        /* En mode CONTINUOUS le SX1262 reste en RX apres chaque paquet ;
         * ce SetRx est defensif (reaffirme l'etat radio apres traitement
         * de l'IRQ). Comportement a confirmer au smoke test materiel
         * (Task 9) : si redondant sans effet de bord, il pourra etre retire. */
        rf_switch_rx(ctx);
        sx126x_set_rx_with_timeout_in_rtc_step(sx, SX126X_RX_CONTINUOUS);

        xSemaphoreGive(ctx->radio_mutex);

        /* Livrer le paquet hors mutex. */
        if (pkt_len > 0 && ctx->rx_cb) {
            ctx->rx_cb(pkt_buf, (size_t)pkt_len, rssi, ctx->rx_user_ctx);
        }
    }

    ESP_LOGI(TAG, "Tache RX arretee");
    vTaskDelete(NULL);
}

/* ================================================================
 * Implementation de la vtable hal_lora_t
 * ================================================================ */

static hal_err_t c1262_init(const hal_lora_config_t *config, void *ctx_ptr)
{
    core1262_ctx_t *ctx = (core1262_ctx_t *)ctx_ptr;

    if (ctx->initialized) {
        return HAL_OK;
    }
    if (!config) {
        return HAL_ERR_INVALID;
    }

    /* 1. Traduire la config HAL en parametres SX1262. */
    if (!core1262_map_config(config, &ctx->params)) {
        ESP_LOGE(TAG, "Config radio invalide");
        return HAL_ERR_INVALID;
    }

    /* 2. GPIO de controle : NSS, RESET, RXEN, TXEN en sortie ; BUSY,
     *    DIO1 en entree. */
    gpio_config_t out_cfg = {
        .pin_bit_mask = (1ULL << ctx->hw.pin_nss)  |
                        (1ULL << ctx->hw.pin_reset) |
                        (1ULL << ctx->pin_rxen)     |
                        (1ULL << ctx->pin_txen),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    if (gpio_config(&out_cfg) != ESP_OK) return HAL_ERR_IO;
    gpio_set_level(ctx->hw.pin_nss, 1);   /* CS au repos = haut */

    gpio_config_t busy_cfg = {
        .pin_bit_mask = (1ULL << ctx->hw.pin_busy),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    if (gpio_config(&busy_cfg) != ESP_OK) return HAL_ERR_IO;

    gpio_config_t dio1_cfg = {
        .pin_bit_mask = (1ULL << ctx->pin_dio1),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_POSEDGE,   /* IRQ SX1262 = front montant */
    };
    if (gpio_config(&dio1_cfg) != ESP_OK) return HAL_ERR_IO;

    /* 3. Bus + device SPI. CS gere a la main (spics_io_num = -1). */
    spi_bus_config_t bus_cfg = {
        .mosi_io_num     = ctx->pin_mosi,
        .miso_io_num     = ctx->pin_miso,
        .sclk_io_num     = ctx->pin_sck,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = HAL_LORA_MAX_PACKET_SIZE + 16,
    };
    esp_err_t err = spi_bus_initialize(ctx->spi_host, &bus_cfg, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_initialize echoue: %d", err);
        return HAL_ERR_IO;
    }

    spi_device_interface_config_t dev_cfg = {
        .clock_speed_hz = C1262_SPI_CLOCK_HZ,
        .mode           = 0,            /* SX1262 : SPI mode 0 */
        .spics_io_num   = -1,           /* CS pilote a la main dans la glue */
        .queue_size     = 1,
    };
    err = spi_bus_add_device(ctx->spi_host, &dev_cfg, &ctx->hw.spi);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_add_device echoue: %d", err);
        spi_bus_free(ctx->spi_host);
        return HAL_ERR_IO;
    }

    /* rc est utilise par les chemins d'erreur goto pour propager le code
     * d'erreur jusqu'aux labels de nettoyage. Initialise a HAL_ERR_IO
     * (valeur la plus courante en cas d'echec materiel/SPI). */
    hal_err_t rc = HAL_ERR_IO;

    /* 4. Reset materiel du SX1262. */
    if (sx126x_hal_reset(&ctx->hw) != SX126X_HAL_STATUS_OK) {
        ESP_LOGE(TAG, "Reset SX1262 echoue (BUSY bloque ?)");
        rc = HAL_ERR_IO;
        goto fail_spi;
    }

    /* 5. Sequence de configuration radio. */
    rc = apply_radio_config(ctx);
    if (rc != HAL_OK) {
        ESP_LOGE(TAG, "Configuration radio echouee");
        goto fail_spi;
    }

    /* 6. Mutex + semaphore + ISR DIO1. */
    ctx->radio_mutex = xSemaphoreCreateMutex();
    ctx->dio1_sem    = xSemaphoreCreateBinary();
    if (!ctx->radio_mutex || !ctx->dio1_sem) {
        ESP_LOGE(TAG, "Creation des primitives FreeRTOS echouee");
        rc = HAL_ERR_NO_MEM;
        goto fail_isr;
    }

    /* gpio_install_isr_service peut deja avoir ete appele par un autre
     * driver (ex: tactile). ESP_ERR_INVALID_STATE = deja installe, OK. */
    err = gpio_install_isr_service(0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "gpio_install_isr_service echoue: %d", err);
        rc = HAL_ERR_IO;
        goto fail_isr;
    }
    err = gpio_isr_handler_add(ctx->pin_dio1, dio1_isr_handler, ctx);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "gpio_isr_handler_add echoue: %d", err);
        rc = HAL_ERR_IO;
        goto fail_isr;
    }

    /* Switch RF au repos en reception. */
    rf_switch_rx(ctx);

    ctx->initialized = true;
    ctx->rx_running  = false;
    ESP_LOGI(TAG, "Core1262 initialise (%lu Hz, SF%u, %d dBm)",
             (unsigned long)ctx->params.freq_hz,
             (unsigned)ctx->params.mod.sf,
             (int)ctx->params.power_dbm);
    return HAL_OK;

fail_isr:
    /* Les semaphores ont ete crees : les liberer (gardes par if pour
     * tolerer une creation partielle, ex: radio_mutex OK mais dio1_sem NULL). */
    if (ctx->dio1_sem)    { vSemaphoreDelete(ctx->dio1_sem);    ctx->dio1_sem    = NULL; }
    if (ctx->radio_mutex) { vSemaphoreDelete(ctx->radio_mutex); ctx->radio_mutex = NULL; }
    /* Tomber sur fail_spi pour liberer aussi le bus SPI. */

fail_spi:
    /* Le bus + device SPI ont ete crees : les liberer. */
    spi_bus_remove_device(ctx->hw.spi);
    spi_bus_free(ctx->spi_host);
    ctx->hw.spi = NULL;
    return rc;
}

static hal_err_t c1262_send(const uint8_t *data, size_t len, void *ctx_ptr)
{
    core1262_ctx_t *ctx = (core1262_ctx_t *)ctx_ptr;

    if (!ctx->initialized)               return HAL_FAIL;
    if (!data || len == 0)               return HAL_ERR_INVALID;
    if (len > HAL_LORA_MAX_PACKET_SIZE)  return HAL_ERR_INVALID;

    if (xSemaphoreTake(ctx->radio_mutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
        ESP_LOGE(TAG, "send : mutex radio indisponible");
        return HAL_ERR_BUSY;
    }

    const void *sx = &ctx->hw;
    hal_err_t   result = HAL_OK;

    /* 1. Ecrire la charge utile dans la FIFO. */
    if (sx126x_write_buffer(sx, 0x00, data, (uint8_t)len) != SX126X_STATUS_OK) {
        result = HAL_ERR_IO;
        goto done;
    }

    /* 2. Renseigner la taille du paquet dans les parametres LoRa. */
    ctx->params.pkt.pld_len_in_bytes = (uint8_t)len;
    if (sx126x_set_lora_pkt_params(sx, &ctx->params.pkt) != SX126X_STATUS_OK) {
        result = HAL_ERR_IO;
        goto done;
    }

    /* 3. Effacer les IRQ residuelles, basculer le switch en TX, emettre. */
    sx126x_clear_irq_status(sx, SX126X_IRQ_TX_DONE | SX126X_IRQ_TIMEOUT);
    rf_switch_tx(ctx);
    /* timeout chip = 0 : pas de timeout materiel. La protection contre un
     * SX1262 bloque en TX est assuree par le timeout logiciel ci-dessous
     * (boucle de scrutation, C1262_TX_TIMEOUT_MS). */
    if (sx126x_set_tx(sx, 0) != SX126X_STATUS_OK) {
        result = HAL_ERR_IO;
        goto done;
    }

    /* 4. Attendre TX_DONE en scrutant le registre IRQ. */
    {
        int  waited_ms = 0;
        bool tx_done   = false;
        while (waited_ms < C1262_TX_TIMEOUT_MS) {
            sx126x_irq_mask_t irq = 0;
            if (sx126x_get_irq_status(sx, &irq) != SX126X_STATUS_OK) {
                result = HAL_ERR_IO;
                goto done;
            }
            if (irq & SX126X_IRQ_TX_DONE) { tx_done = true; break; }
            if (irq & SX126X_IRQ_TIMEOUT)  { break; }
            vTaskDelay(pdMS_TO_TICKS(5));
            waited_ms += 5;
        }
        sx126x_clear_irq_status(sx, SX126X_IRQ_TX_DONE | SX126X_IRQ_TIMEOUT);

        if (!tx_done) {
            ESP_LOGW(TAG, "send : TX_DONE non recu (%d octets)", (int)len);
            result = HAL_ERR_TIMEOUT;
        }
    }

done:
    /* Toujours rebasculer en reception continue si la tache RX tourne.
     * On utilise sx126x_set_rx_with_timeout_in_rtc_step avec
     * SX126X_RX_CONTINUOUS (0x00FFFFFF) pour la reception permanente. */
    rf_switch_rx(ctx);
    if (ctx->rx_running) {
        sx126x_set_rx_with_timeout_in_rtc_step(sx, SX126X_RX_CONTINUOUS);
    }
    xSemaphoreGive(ctx->radio_mutex);
    return result;
}

static hal_err_t c1262_set_rx_callback(hal_lora_rx_cb_t cb, void *user_ctx,
                                       void *ctx_ptr)
{
    core1262_ctx_t *ctx = (core1262_ctx_t *)ctx_ptr;
    ctx->rx_cb       = cb;
    ctx->rx_user_ctx = user_ctx;
    return HAL_OK;
}

static hal_err_t c1262_start_rx(void *ctx_ptr)
{
    core1262_ctx_t *ctx = (core1262_ctx_t *)ctx_ptr;

    if (!ctx->initialized) return HAL_FAIL;
    if (!ctx->rx_cb)       return HAL_ERR_INVALID;
    if (ctx->rx_running)   return HAL_OK;

    ctx->rx_running = true;

    BaseType_t ok = xTaskCreate(c1262_rx_task, "c1262_rx", C1262_RX_TASK_STACK,
                                ctx, C1262_RX_TASK_PRIO, &ctx->rx_task_handle);
    if (ok != pdPASS) {
        ctx->rx_running = false;
        ESP_LOGE(TAG, "Creation tache RX echouee");
        return HAL_ERR_NO_MEM;
    }

    /* Lancer la reception continue. */
    if (xSemaphoreTake(ctx->radio_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        ESP_LOGE(TAG, "start_rx : impossible d'armer la reception (mutex indisponible)");
        ctx->rx_running = false;
        vTaskDelete(ctx->rx_task_handle);
        ctx->rx_task_handle = NULL;
        return HAL_ERR_BUSY;
    }
    rf_switch_rx(ctx);
    sx126x_set_rx_with_timeout_in_rtc_step(&ctx->hw, SX126X_RX_CONTINUOUS);
    xSemaphoreGive(ctx->radio_mutex);

    ESP_LOGI(TAG, "Mode reception active");
    return HAL_OK;
}

static hal_err_t c1262_sleep(void *ctx_ptr)
{
    core1262_ctx_t *ctx = (core1262_ctx_t *)ctx_ptr;

    if (!ctx->initialized) return HAL_OK;

    /* Arreter la tache RX. */
    if (ctx->rx_running) {
        ctx->rx_running = false;
        vTaskDelay(pdMS_TO_TICKS(600));   /* laisser la tache sortir de sa boucle */
    }

    /* Mettre le SX1262 en standby basse conso. */
    if (xSemaphoreTake(ctx->radio_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
        sx126x_set_standby(&ctx->hw, SX126X_STANDBY_CFG_RC);
        xSemaphoreGive(ctx->radio_mutex);
    }

    ESP_LOGI(TAG, "Core1262 en veille");
    return HAL_OK;
}

/* ================================================================
 * Factory
 * ================================================================ */

hal_err_t hal_lora_core1262_create(hal_lora_t *lora,
                                   const hal_lora_core1262_pins_t *pins)
{
    if (!lora || !pins) {
        return HAL_ERR_INVALID;
    }

    memset(&s_ctx, 0, sizeof(s_ctx));
    s_ctx.hw.pin_nss   = pins->pin_nss;
    s_ctx.hw.pin_busy  = pins->pin_busy;
    s_ctx.hw.pin_reset = pins->pin_reset;
    s_ctx.pin_dio1     = pins->pin_dio1;
    s_ctx.pin_rxen     = pins->pin_rxen;
    s_ctx.pin_txen     = pins->pin_txen;
    s_ctx.spi_host     = pins->spi_host;
    s_ctx.pin_sck      = pins->pin_sck;
    s_ctx.pin_mosi     = pins->pin_mosi;
    s_ctx.pin_miso     = pins->pin_miso;

    lora->init            = c1262_init;
    lora->send            = c1262_send;
    lora->set_rx_callback = c1262_set_rx_callback;
    lora->start_rx        = c1262_start_rx;
    lora->sleep           = c1262_sleep;
    lora->ctx             = &s_ctx;

    ESP_LOGI(TAG, "Driver cree (SPI%d, NSS=%d, DIO1=%d)",
             pins->spi_host, pins->pin_nss, pins->pin_dio1);
    return HAL_OK;
}
