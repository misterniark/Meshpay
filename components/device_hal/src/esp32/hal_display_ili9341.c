/**
 * @file hal_display_ili9341.c
 * @brief Driver ILI9341 (SPI) + XPT2046 (tactile) pour CYD (ESP32-2432S028).
 *
 * Ce fichier implémente l'interface hal_display_t pour le module CYD :
 * - Écran ILI9341 320x240 RGB565 sur SPI2 (HSPI)
 * - Contrôleur tactile résistif XPT2046 sur SPI3 (VSPI)
 * - Rétroéclairage PWM via LEDC sur GPIO 21
 *
 * Pinout CYD (ESP32-2432S028) :
 *   LCD  : MOSI=13, SCK=14, CS=15, DC=2, RST=-1
 *   Touch: MOSI=32, MISO=39, SCK=25, CS=33, IRQ=36
 *   BL   : GPIO 21
 */

#include "hal_display_ili9341.h"
#include "esp_log.h"

#include "driver/spi_master.h"
#include "driver/ledc.h"
#include "driver/gpio.h"

#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/*                       Constantes et macros                         */
/* ------------------------------------------------------------------ */

static const char *TAG = "hal_display_ili9341";

/* Résolution fixe du CYD en mode paysage */
#define CYD_WIDTH  320
#define CYD_HEIGHT 240

/* --- Broches LCD (SPI2 / HSPI) --- */
#define LCD_PIN_MOSI  13
#define LCD_PIN_SCK   14
#define LCD_PIN_CS    15
#define LCD_PIN_DC     2  /* Data/Command : 0=commande, 1=données */
#define LCD_PIN_RST   (-1) /* Pas de broche reset dédiée */

/* --- Broches rétroéclairage --- */
#define BL_PIN        21

/* --- Broches tactile XPT2046 (SPI3 / VSPI) --- */
#define TOUCH_PIN_MOSI 32
#define TOUCH_PIN_MISO 39
#define TOUCH_PIN_SCK  25
#define TOUCH_PIN_CS   33
#define TOUCH_PIN_IRQ  36

/* --- Configuration SPI --- */
#define LCD_SPI_HOST   SPI2_HOST
#define LCD_CLOCK_HZ   (40 * 1000 * 1000) /* 40 MHz pour les données */
#define LCD_CMD_CLK_HZ (26 * 1000 * 1000) /* 26 MHz pour les commandes */

#define TOUCH_SPI_HOST SPI3_HOST
#define TOUCH_CLOCK_HZ (2 * 1000 * 1000)  /* 2 MHz */

/* --- Configuration LEDC pour le rétroéclairage --- */
#define BL_LEDC_TIMER   LEDC_TIMER_0
#define BL_LEDC_CHANNEL LEDC_CHANNEL_0
#define BL_LEDC_MODE    LEDC_LOW_SPEED_MODE
#define BL_LEDC_FREQ_HZ 5000   /* 5 kHz : inaudible, bon compromis */
#define BL_LEDC_DUTY_RES LEDC_TIMER_8_BIT /* Résolution 8 bits (0-255) */

/* --- Calibration tactile XPT2046 --- */
/* Valeurs brutes min/max observées sur le CYD */
#define TOUCH_RAW_MIN_X 200
#define TOUCH_RAW_MAX_X 3900
#define TOUCH_RAW_MIN_Y 200
#define TOUCH_RAW_MAX_Y 3900

/* Nombre de lectures pour le moyennage (on enlève min et max) */
#define TOUCH_SAMPLES   3

/* --- Commandes XPT2046 --- */
#define XPT2046_CMD_X   0xD0  /* Lire coordonnée X (12 bits, mode différentiel) */
#define XPT2046_CMD_Y   0x90  /* Lire coordonnée Y (12 bits, mode différentiel) */

/* --- Commandes ILI9341 --- */
#define ILI9341_CMD_SWRESET   0x01  /* Software Reset */
#define ILI9341_CMD_SLPOUT    0x11  /* Sleep Out */
#define ILI9341_CMD_DISPON    0x29  /* Display ON */
#define ILI9341_CMD_CASET     0x2A  /* Column Address Set */
#define ILI9341_CMD_PASET     0x2B  /* Page Address Set */
#define ILI9341_CMD_RAMWR     0x2C  /* Memory Write */
#define ILI9341_CMD_MADCTL    0x3A  /* Pixel Format Set — en fait c'est COLMOD */
#define ILI9341_CMD_COLMOD    0x3A  /* Interface Pixel Format */
#define ILI9341_CMD_MADCTL_R  0x36  /* Memory Access Control (orientation) */
#define ILI9341_CMD_PWCTR1    0xC0  /* Power Control 1 */
#define ILI9341_CMD_PWCTR2    0xC1  /* Power Control 2 */
#define ILI9341_CMD_VMCTR1    0xC5  /* VCOM Control 1 */
#define ILI9341_CMD_VMCTR2    0xC7  /* VCOM Control 2 */
#define ILI9341_CMD_GMCTRP1   0xE0  /* Positive Gamma Correction */
#define ILI9341_CMD_GMCTRN1   0xE1  /* Negative Gamma Correction */

/* Valeur MADCTL pour orientation paysage (320x240) :
 * Bit 5 (MV) = 1 : échange lignes/colonnes
 * Bit 3 (BGR) = 1 : ordre BGR
 * => 0x28 */
#define ILI9341_MADCTL_LANDSCAPE 0x28

/* Taille maximale d'une transaction SPI DMA en octets (4092 est un
 * multiple de 4 compatible avec les contraintes DMA de l'ESP32) */
#define SPI_MAX_TRANSFER_SIZE 4092

/* ------------------------------------------------------------------ */
/*                      Structure de contexte                         */
/* ------------------------------------------------------------------ */

/**
 * Contexte privé du driver ILI9341 + XPT2046.
 *
 * Contient les handles SPI pour l'écran et le tactile.
 * Alloué dynamiquement dans hal_display_ili9341_create().
 */
typedef struct {
    spi_device_handle_t lcd_spi;   /* Handle SPI de l'écran ILI9341 */
    spi_device_handle_t touch_spi; /* Handle SPI du contrôleur tactile XPT2046 */
} ili9341_ctx_t;

/* ------------------------------------------------------------------ */
/*                    Fonctions SPI LCD (privées)                     */
/* ------------------------------------------------------------------ */

/**
 * Envoie une commande (1 octet) à l'ILI9341.
 *
 * La broche DC est mise à 0 (commande) via le champ user du descripteur
 * de transaction SPI. Le pre_transfer_callback s'en charge.
 *
 * @param ctx Contexte du driver
 * @param cmd Octet de commande ILI9341
 * @return HAL_OK ou HAL_FAIL
 */
static hal_err_t lcd_send_cmd(ili9341_ctx_t *ctx, uint8_t cmd)
{
    spi_transaction_t t = {
        .length    = 8,        /* 1 octet = 8 bits */
        .tx_buffer = &cmd,
        .user      = (void *)0 /* DC=0 → commande */
    };

    esp_err_t err = spi_device_polling_transmit(ctx->lcd_spi, &t);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "lcd_send_cmd(0x%02X) échoué : %s", cmd, esp_err_to_name(err));
        return HAL_FAIL;
    }
    return HAL_OK;
}

/**
 * Envoie des données à l'ILI9341 via polling (petits paquets).
 *
 * Utilisé pour les paramètres de commandes (quelques octets).
 * La broche DC est mise à 1 (données).
 *
 * @param ctx  Contexte du driver
 * @param data Tampon de données à envoyer
 * @param len  Nombre d'octets
 * @return HAL_OK ou HAL_FAIL
 */
static hal_err_t lcd_send_data(ili9341_ctx_t *ctx, const uint8_t *data, size_t len)
{
    if (len == 0) {
        return HAL_OK;
    }

    spi_transaction_t t = {
        .length    = len * 8,  /* Longueur en bits */
        .tx_buffer = data,
        .user      = (void *)1 /* DC=1 → données */
    };

    esp_err_t err = spi_device_polling_transmit(ctx->lcd_spi, &t);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "lcd_send_data échoué : %s", esp_err_to_name(err));
        return HAL_FAIL;
    }
    return HAL_OK;
}

/**
 * Callback appelé avant chaque transaction SPI LCD.
 *
 * Positionne la broche DC (Data/Command) selon le champ user :
 * - user == 0 → commande (DC bas)
 * - user != 0 → données (DC haut)
 */
static void IRAM_ATTR lcd_spi_pre_transfer_cb(spi_transaction_t *t)
{
    int dc_level = (int)t->user;
    gpio_set_level(LCD_PIN_DC, dc_level);
}

/* ------------------------------------------------------------------ */
/*                   Séquence d'initialisation ILI9341                */
/* ------------------------------------------------------------------ */

/**
 * Structure décrivant une commande d'initialisation ILI9341.
 *
 * Permet de définir la séquence sous forme de tableau statique.
 */
typedef struct {
    uint8_t cmd;            /* Code de la commande */
    uint8_t data[16];       /* Données associées (max 16 octets) */
    uint8_t data_len;       /* Nombre d'octets de données */
    uint16_t delay_ms;      /* Délai après la commande (0 = pas de délai) */
} ili9341_init_cmd_t;

/**
 * Séquence d'initialisation complète de l'ILI9341.
 *
 * Inclut : reset logiciel, contrôle d'alimentation, VCOM, gamma,
 * orientation paysage, format pixel 16 bits, sortie de veille,
 * et activation de l'affichage.
 */
static const ili9341_init_cmd_t ili9341_init_cmds[] = {
    /* Reset logiciel */
    { ILI9341_CMD_SWRESET, {0}, 0, 150 },

    /* Power Control 1 : tension GVDD = 4.60V */
    { ILI9341_CMD_PWCTR1, {0x23}, 1, 0 },

    /* Power Control 2 : facteur de multiplication pour le step-up */
    { ILI9341_CMD_PWCTR2, {0x10}, 1, 0 },

    /* VCOM Control 1 : réglage de la tension VCOM */
    { ILI9341_CMD_VMCTR1, {0x3E, 0x28}, 2, 0 },

    /* VCOM Control 2 : offset VCOM */
    { ILI9341_CMD_VMCTR2, {0x86}, 1, 0 },

    /* Memory Access Control : orientation paysage + BGR */
    { ILI9341_CMD_MADCTL_R, {ILI9341_MADCTL_LANDSCAPE}, 1, 0 },

    /* Pixel Format : 16 bits par pixel (RGB565) */
    { ILI9341_CMD_COLMOD, {0x55}, 1, 0 },

    /* Positive Gamma Correction */
    { ILI9341_CMD_GMCTRP1,
      {0x0F, 0x31, 0x2B, 0x0C, 0x0E, 0x08,
       0x4E, 0xF1, 0x37, 0x07, 0x10, 0x03,
       0x0E, 0x09, 0x00}, 15, 0 },

    /* Negative Gamma Correction */
    { ILI9341_CMD_GMCTRN1,
      {0x00, 0x0E, 0x14, 0x03, 0x11, 0x07,
       0x31, 0xC1, 0x48, 0x08, 0x0F, 0x0C,
       0x31, 0x36, 0x0F}, 15, 0 },

    /* Sleep Out : sortie du mode veille */
    { ILI9341_CMD_SLPOUT, {0}, 0, 120 },

    /* Display ON : activation de l'affichage */
    { ILI9341_CMD_DISPON, {0}, 0, 0 },
};

/**
 * Exécute la séquence d'initialisation complète de l'ILI9341.
 *
 * Parcourt le tableau de commandes et envoie chaque commande
 * avec ses données et le délai associé.
 *
 * @param ctx Contexte du driver
 * @return HAL_OK ou HAL_FAIL
 */
static hal_err_t lcd_init_sequence(ili9341_ctx_t *ctx)
{
    const size_t num_cmds = sizeof(ili9341_init_cmds) / sizeof(ili9341_init_cmds[0]);

    for (size_t i = 0; i < num_cmds; i++) {
        const ili9341_init_cmd_t *entry = &ili9341_init_cmds[i];

        /* Envoi de la commande */
        hal_err_t err = lcd_send_cmd(ctx, entry->cmd);
        if (err != HAL_OK) {
            ESP_LOGE(TAG, "Échec commande init 0x%02X (étape %d/%d)",
                     entry->cmd, (int)i + 1, (int)num_cmds);
            return err;
        }

        /* Envoi des données associées, s'il y en a */
        if (entry->data_len > 0) {
            err = lcd_send_data(ctx, entry->data, entry->data_len);
            if (err != HAL_OK) {
                ESP_LOGE(TAG, "Échec données init commande 0x%02X", entry->cmd);
                return err;
            }
        }

        /* Délai post-commande si nécessaire (ex: après SWRESET, SLPOUT) */
        if (entry->delay_ms > 0) {
            vTaskDelay(pdMS_TO_TICKS(entry->delay_ms));
        }
    }

    ESP_LOGI(TAG, "Séquence d'initialisation ILI9341 terminée (%d commandes)", (int)num_cmds);
    return HAL_OK;
}

/* ------------------------------------------------------------------ */
/*                   Définition de la fenêtre d'adresse               */
/* ------------------------------------------------------------------ */

/**
 * Définit la zone rectangulaire de l'écran qui recevra les pixels.
 *
 * Envoie les commandes CASET (colonnes) et PASET (pages) suivies
 * de RAMWR pour préparer l'écriture en mémoire vidéo.
 *
 * @param ctx Contexte du driver
 * @param x1  Colonne de début (incluse)
 * @param y1  Ligne de début (incluse)
 * @param x2  Colonne de fin (incluse)
 * @param y2  Ligne de fin (incluse)
 * @return HAL_OK ou HAL_FAIL
 */
static hal_err_t lcd_set_address_window(ili9341_ctx_t *ctx,
                                        uint16_t x1, uint16_t y1,
                                        uint16_t x2, uint16_t y2)
{
    /* CASET : Column Address Set */
    uint8_t col_data[4] = {
        (uint8_t)(x1 >> 8), (uint8_t)(x1 & 0xFF),
        (uint8_t)(x2 >> 8), (uint8_t)(x2 & 0xFF)
    };
    hal_err_t err = lcd_send_cmd(ctx, ILI9341_CMD_CASET);
    if (err != HAL_OK) return err;
    err = lcd_send_data(ctx, col_data, 4);
    if (err != HAL_OK) return err;

    /* PASET : Page Address Set */
    uint8_t row_data[4] = {
        (uint8_t)(y1 >> 8), (uint8_t)(y1 & 0xFF),
        (uint8_t)(y2 >> 8), (uint8_t)(y2 & 0xFF)
    };
    err = lcd_send_cmd(ctx, ILI9341_CMD_PASET);
    if (err != HAL_OK) return err;
    err = lcd_send_data(ctx, row_data, 4);
    if (err != HAL_OK) return err;

    /* RAMWR : prépare l'écriture en mémoire */
    err = lcd_send_cmd(ctx, ILI9341_CMD_RAMWR);
    return err;
}

/* ------------------------------------------------------------------ */
/*                    Fonctions tactile XPT2046                       */
/* ------------------------------------------------------------------ */

/**
 * Lit une valeur brute 12 bits depuis le XPT2046.
 *
 * Envoie une commande (1 octet) et lit la réponse (2 octets).
 * Le résultat est un entier 12 bits (0-4095).
 *
 * @param ctx Contexte du driver
 * @param cmd Commande XPT2046 (XPT2046_CMD_X ou XPT2046_CMD_Y)
 * @return Valeur brute 12 bits, ou -1 en cas d'erreur
 */
static int32_t touch_read_raw(ili9341_ctx_t *ctx, uint8_t cmd)
{
    uint8_t tx_buf[3] = { cmd, 0x00, 0x00 };
    uint8_t rx_buf[3] = { 0 };

    spi_transaction_t t = {
        .length    = 24,       /* 3 octets = 24 bits */
        .tx_buffer = tx_buf,
        .rx_buffer = rx_buf,
        .flags     = 0
    };

    esp_err_t err = spi_device_polling_transmit(ctx->touch_spi, &t);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "touch_read_raw échoué : %s", esp_err_to_name(err));
        return -1;
    }

    /* Les 12 bits utiles sont dans rx_buf[1] (bits 7..0) et rx_buf[2] (bits 7..4) */
    int32_t raw = ((int32_t)rx_buf[1] << 4) | ((int32_t)rx_buf[2] >> 4);
    return raw;
}

/**
 * Lit une coordonnée tactile avec moyennage robuste.
 *
 * Effectue TOUCH_SAMPLES lectures, enlève la valeur minimale et
 * maximale, puis retourne la moyenne des valeurs restantes.
 * Cette méthode élimine les pics de bruit.
 *
 * @param ctx Contexte du driver
 * @param cmd Commande XPT2046 (X ou Y)
 * @return Valeur moyennée, ou -1 en cas d'erreur
 */
static int32_t touch_read_filtered(ili9341_ctx_t *ctx, uint8_t cmd)
{
    int32_t samples[TOUCH_SAMPLES];

    /* Collecte des échantillons */
    for (int i = 0; i < TOUCH_SAMPLES; i++) {
        samples[i] = touch_read_raw(ctx, cmd);
        if (samples[i] < 0) {
            return -1; /* Erreur SPI */
        }
    }

    /* Recherche du min et du max pour les exclure */
    int32_t min_val = samples[0];
    int32_t max_val = samples[0];
    int32_t sum = samples[0];

    for (int i = 1; i < TOUCH_SAMPLES; i++) {
        if (samples[i] < min_val) min_val = samples[i];
        if (samples[i] > max_val) max_val = samples[i];
        sum += samples[i];
    }

    /* Moyenne sans le min et le max.
     * Avec 3 échantillons, on obtient la médiane. */
    int32_t filtered = sum - min_val - max_val;
    /* Nombre de valeurs restantes après exclusion min/max */
    int count = TOUCH_SAMPLES - 2;
    if (count <= 0) {
        /* Cas dégénéré : avec seulement 1 ou 2 échantillons */
        return sum / TOUCH_SAMPLES;
    }

    return filtered / count;
}

/**
 * Convertit une valeur brute tactile en coordonnée pixel.
 *
 * Applique une calibration linéaire : raw_min..raw_max → 0..pixel_max.
 * Les valeurs sont saturées aux bornes.
 *
 * @param raw       Valeur brute du XPT2046
 * @param raw_min   Valeur brute minimale calibrée
 * @param raw_max   Valeur brute maximale calibrée
 * @param pixel_max Coordonnée pixel maximale (ex: 319 pour X)
 * @return Coordonnée pixel (0..pixel_max)
 */
static uint16_t touch_calibrate(int32_t raw, int32_t raw_min,
                                int32_t raw_max, uint16_t pixel_max)
{
    /* Saturation aux bornes de calibration */
    if (raw <= raw_min) return 0;
    if (raw >= raw_max) return pixel_max;

    /* Interpolation linéaire */
    int32_t pixel = (raw - raw_min) * (int32_t)pixel_max / (raw_max - raw_min);

    /* Sécurité : saturation finale */
    if (pixel < 0) return 0;
    if (pixel > (int32_t)pixel_max) return pixel_max;

    return (uint16_t)pixel;
}

/* ------------------------------------------------------------------ */
/*              Implémentation des fonctions HAL                      */
/* ------------------------------------------------------------------ */

/**
 * Initialise l'écran ILI9341, le tactile XPT2046 et le rétroéclairage.
 *
 * Cette fonction :
 * 1. Configure la broche DC en sortie GPIO
 * 2. Configure la broche IRQ du tactile en entrée
 * 3. Initialise le bus SPI de l'écran (SPI2/HSPI)
 * 4. Ajoute le device LCD sur le bus SPI
 * 5. Initialise le bus SPI du tactile (SPI3/VSPI)
 * 6. Ajoute le device tactile sur le bus SPI
 * 7. Configure le rétroéclairage PWM via LEDC
 * 8. Exécute la séquence d'initialisation de l'ILI9341
 * 9. Allume le rétroéclairage à 100%
 *
 * @param ctx Contexte opaque (ili9341_ctx_t *)
 * @return HAL_OK en cas de succès, HAL_FAIL sinon
 */
static hal_err_t ili9341_init(void *ctx)
{
    ili9341_ctx_t *drv = (ili9341_ctx_t *)ctx;
    esp_err_t ret;

    /* ---- 1. Configuration GPIO DC (Data/Command) ---- */
    gpio_config_t dc_conf = {
        .pin_bit_mask = (1ULL << LCD_PIN_DC),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    ret = gpio_config(&dc_conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Configuration GPIO DC échouée : %s", esp_err_to_name(ret));
        return HAL_FAIL;
    }

    /* ---- 2. Configuration GPIO IRQ tactile (entrée) ---- */
    gpio_config_t irq_conf = {
        .pin_bit_mask = (1ULL << TOUCH_PIN_IRQ),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    ret = gpio_config(&irq_conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Configuration GPIO IRQ tactile échouée : %s", esp_err_to_name(ret));
        return HAL_FAIL;
    }

    /* ---- 3. Initialisation du bus SPI LCD (SPI2/HSPI) ---- */
    spi_bus_config_t lcd_bus_cfg = {
        .mosi_io_num     = LCD_PIN_MOSI,
        .miso_io_num     = -1,           /* Pas de MISO pour l'écran */
        .sclk_io_num     = LCD_PIN_SCK,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = SPI_MAX_TRANSFER_SIZE,
    };
    ret = spi_bus_initialize(LCD_SPI_HOST, &lcd_bus_cfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Init bus SPI LCD échouée : %s", esp_err_to_name(ret));
        return HAL_FAIL;
    }

    /* ---- 4. Ajout du device LCD sur le bus SPI ---- */
    spi_device_interface_config_t lcd_dev_cfg = {
        .clock_speed_hz = LCD_CLOCK_HZ,
        .mode           = 0,              /* SPI mode 0 (CPOL=0, CPHA=0) */
        .spics_io_num   = LCD_PIN_CS,
        .queue_size     = 7,              /* Profondeur de la file DMA */
        .pre_cb         = lcd_spi_pre_transfer_cb, /* Gestion de DC */
    };
    ret = spi_bus_add_device(LCD_SPI_HOST, &lcd_dev_cfg, &drv->lcd_spi);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Ajout device SPI LCD échoué : %s", esp_err_to_name(ret));
        spi_bus_free(LCD_SPI_HOST);
        return HAL_FAIL;
    }

    /* ---- 5. Initialisation du bus SPI tactile (SPI3/VSPI) ---- */
    spi_bus_config_t touch_bus_cfg = {
        .mosi_io_num     = TOUCH_PIN_MOSI,
        .miso_io_num     = TOUCH_PIN_MISO,
        .sclk_io_num     = TOUCH_PIN_SCK,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = 32,  /* Le tactile n'envoie que quelques octets */
    };
    ret = spi_bus_initialize(TOUCH_SPI_HOST, &touch_bus_cfg, SPI_DMA_DISABLED);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Init bus SPI tactile échouée : %s", esp_err_to_name(ret));
        spi_bus_remove_device(drv->lcd_spi);
        spi_bus_free(LCD_SPI_HOST);
        return HAL_FAIL;
    }

    /* ---- 6. Ajout du device tactile sur le bus SPI ---- */
    spi_device_interface_config_t touch_dev_cfg = {
        .clock_speed_hz = TOUCH_CLOCK_HZ,
        .mode           = 0,              /* SPI mode 0 */
        .spics_io_num   = TOUCH_PIN_CS,
        .queue_size     = 1,
    };
    ret = spi_bus_add_device(TOUCH_SPI_HOST, &touch_dev_cfg, &drv->touch_spi);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Ajout device SPI tactile échoué : %s", esp_err_to_name(ret));
        spi_bus_remove_device(drv->lcd_spi);
        spi_bus_free(LCD_SPI_HOST);
        spi_bus_free(TOUCH_SPI_HOST);
        return HAL_FAIL;
    }

    /* ---- 7. Configuration du rétroéclairage PWM via LEDC ---- */
    ledc_timer_config_t bl_timer = {
        .speed_mode      = BL_LEDC_MODE,
        .timer_num       = BL_LEDC_TIMER,
        .duty_resolution = BL_LEDC_DUTY_RES,
        .freq_hz         = BL_LEDC_FREQ_HZ,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    ret = ledc_timer_config(&bl_timer);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Config timer LEDC échouée : %s", esp_err_to_name(ret));
        return HAL_FAIL;
    }

    ledc_channel_config_t bl_channel = {
        .speed_mode = BL_LEDC_MODE,
        .channel    = BL_LEDC_CHANNEL,
        .timer_sel  = BL_LEDC_TIMER,
        .intr_type  = LEDC_INTR_DISABLE,
        .gpio_num   = BL_PIN,
        .duty       = 0,  /* Éteint au départ */
        .hpoint     = 0,
    };
    ret = ledc_channel_config(&bl_channel);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Config canal LEDC échouée : %s", esp_err_to_name(ret));
        return HAL_FAIL;
    }

    /* ---- 8. Séquence d'initialisation ILI9341 ---- */
    hal_err_t hal_ret = lcd_init_sequence(drv);
    if (hal_ret != HAL_OK) {
        ESP_LOGE(TAG, "Séquence d'init ILI9341 échouée");
        return hal_ret;
    }

    /* ---- 9. Rétroéclairage à 100% ---- */
    ledc_set_duty(BL_LEDC_MODE, BL_LEDC_CHANNEL, 255);
    ledc_update_duty(BL_LEDC_MODE, BL_LEDC_CHANNEL);

    ESP_LOGI(TAG, "ILI9341 + XPT2046 initialisé (320x240, paysage)");
    return HAL_OK;
}

/**
 * Envoie une zone de pixels RGB565 à l'écran.
 *
 * Découpe le transfert en morceaux de SPI_MAX_TRANSFER_SIZE octets
 * pour respecter les contraintes DMA. Les commandes (set address window)
 * sont envoyées en polling, les données pixels en mode file (DMA).
 *
 * @param x1    Colonne de début
 * @param y1    Ligne de début
 * @param x2    Colonne de fin
 * @param y2    Ligne de fin
 * @param color Pixels RGB565
 * @param ctx   Contexte opaque
 * @return HAL_OK ou HAL_FAIL
 */
static hal_err_t ili9341_flush(uint16_t x1, uint16_t y1,
                               uint16_t x2, uint16_t y2,
                               const uint16_t *color, void *ctx)
{
    ili9341_ctx_t *drv = (ili9341_ctx_t *)ctx;

    /* Validation des coordonnées */
    if (x2 >= CYD_WIDTH || y2 >= CYD_HEIGHT || x1 > x2 || y1 > y2) {
        ESP_LOGE(TAG, "flush : coordonnées invalides (%d,%d)-(%d,%d)",
                 x1, y1, x2, y2);
        return HAL_ERR_INVALID;
    }

    if (!color) {
        return HAL_ERR_INVALID;
    }

    /* Définir la fenêtre d'adresse */
    hal_err_t err = lcd_set_address_window(drv, x1, y1, x2, y2);
    if (err != HAL_OK) {
        return err;
    }

    /* Calcul du nombre total d'octets à envoyer */
    size_t total_pixels = (size_t)(x2 - x1 + 1) * (size_t)(y2 - y1 + 1);
    size_t total_bytes = total_pixels * 2; /* RGB565 = 2 octets/pixel */
    const uint8_t *data_ptr = (const uint8_t *)color;

    /* Envoi des données pixels par morceaux via DMA */
    while (total_bytes > 0) {
        size_t chunk = (total_bytes > SPI_MAX_TRANSFER_SIZE)
                       ? SPI_MAX_TRANSFER_SIZE : total_bytes;

        spi_transaction_t t = {
            .length    = chunk * 8,   /* Longueur en bits */
            .tx_buffer = data_ptr,
            .user      = (void *)1,   /* DC=1 → données */
        };

        /* Utilisation de queue_trans + get_trans_result pour le DMA */
        esp_err_t ret = spi_device_queue_trans(drv->lcd_spi, &t, portMAX_DELAY);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "flush queue_trans échoué : %s", esp_err_to_name(ret));
            return HAL_FAIL;
        }

        /* Attente de la fin du transfert DMA */
        spi_transaction_t *ret_trans;
        ret = spi_device_get_trans_result(drv->lcd_spi, &ret_trans, portMAX_DELAY);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "flush get_trans_result échoué : %s", esp_err_to_name(ret));
            return HAL_FAIL;
        }

        data_ptr    += chunk;
        total_bytes -= chunk;
    }

    return HAL_OK;
}

/**
 * Lit l'état actuel du tactile XPT2046.
 *
 * Vérifie d'abord la broche IRQ (active bas = écran touché).
 * Si touché, lit X et Y avec moyennage et calibration.
 * Si non touché, retourne pressed=false avec les dernières
 * coordonnées connues.
 *
 * @param point [out] Position et état de pression
 * @param ctx   Contexte opaque
 * @return HAL_OK ou HAL_FAIL
 */
static hal_err_t ili9341_touch_read(hal_touch_point_t *point, void *ctx)
{
    ili9341_ctx_t *drv = (ili9341_ctx_t *)ctx;

    if (!point) {
        return HAL_ERR_INVALID;
    }

    /* IRQ est active bas : 0 = touché, 1 = pas touché */
    int irq_level = gpio_get_level(TOUCH_PIN_IRQ);
    if (irq_level != 0) {
        /* Écran non touché */
        point->pressed = false;
        point->x = 0;
        point->y = 0;
        return HAL_OK;
    }

    /* Écran touché : lecture avec moyennage */
    int32_t raw_x = touch_read_filtered(drv, XPT2046_CMD_X);
    int32_t raw_y = touch_read_filtered(drv, XPT2046_CMD_Y);

    if (raw_x < 0 || raw_y < 0) {
        ESP_LOGE(TAG, "Erreur lecture tactile");
        point->pressed = false;
        return HAL_FAIL;
    }

    /* Calibration linéaire : valeurs brutes → coordonnées pixels */
    point->x = touch_calibrate(raw_x, TOUCH_RAW_MIN_X, TOUCH_RAW_MAX_X,
                               CYD_WIDTH - 1);
    point->y = touch_calibrate(raw_y, TOUCH_RAW_MIN_Y, TOUCH_RAW_MAX_Y,
                               CYD_HEIGHT - 1);
    point->pressed = true;

    return HAL_OK;
}

/**
 * Règle la luminosité du rétroéclairage via PWM LEDC.
 *
 * @param brightness Luminosité de 0 (éteint) à 100 (maximum)
 * @param ctx        Contexte opaque
 * @return HAL_OK
 */
static hal_err_t ili9341_set_backlight(uint8_t brightness, void *ctx)
{
    (void)ctx;

    /* Saturation à 100% */
    if (brightness > 100) {
        brightness = 100;
    }

    /* Conversion 0-100 → 0-255 (résolution 8 bits du timer LEDC) */
    uint32_t duty = (uint32_t)brightness * 255 / 100;

    esp_err_t ret = ledc_set_duty(BL_LEDC_MODE, BL_LEDC_CHANNEL, duty);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "set_backlight ledc_set_duty échoué : %s", esp_err_to_name(ret));
        return HAL_FAIL;
    }

    ret = ledc_update_duty(BL_LEDC_MODE, BL_LEDC_CHANNEL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "set_backlight ledc_update_duty échoué : %s", esp_err_to_name(ret));
        return HAL_FAIL;
    }

    ESP_LOGD(TAG, "Rétroéclairage réglé à %d%% (duty=%lu)", brightness, (unsigned long)duty);
    return HAL_OK;
}

/**
 * Retourne la résolution de l'écran en mode paysage.
 *
 * @param width  [out] Largeur en pixels (320)
 * @param height [out] Hauteur en pixels (240)
 * @param ctx    Contexte opaque (non utilisé)
 * @return HAL_OK
 */
static hal_err_t ili9341_get_resolution(uint16_t *width, uint16_t *height,
                                        void *ctx)
{
    (void)ctx;

    if (width)  *width  = CYD_WIDTH;
    if (height) *height = CYD_HEIGHT;

    return HAL_OK;
}

/* ------------------------------------------------------------------ */
/*                         Factory function                           */
/* ------------------------------------------------------------------ */

/**
 * Crée une instance du driver ILI9341 + XPT2046 pour le CYD.
 *
 * Alloue le contexte privé (ili9341_ctx_t) et remplit la vtable
 * hal_display_t avec les pointeurs de fonctions du driver.
 *
 * L'appelant doit ensuite appeler display->init(display->ctx) pour
 * initialiser le matériel.
 *
 * @param display [out] Vtable à remplir
 * @return HAL_OK en cas de succès, HAL_ERR_INVALID si display est NULL,
 *         HAL_ERR_NO_MEM si l'allocation échoue
 */
hal_err_t hal_display_ili9341_create(hal_display_t *display)
{
    if (!display) {
        return HAL_ERR_INVALID;
    }

    /* Allocation du contexte privé */
    ili9341_ctx_t *drv = calloc(1, sizeof(ili9341_ctx_t));
    if (!drv) {
        ESP_LOGE(TAG, "Allocation du contexte échouée");
        return HAL_ERR_NO_MEM;
    }

    /* Remplissage de la vtable */
    display->init           = ili9341_init;
    display->flush          = ili9341_flush;
    display->touch_read     = ili9341_touch_read;
    display->set_backlight  = ili9341_set_backlight;
    display->get_resolution = ili9341_get_resolution;
    display->ctx            = drv;

    ESP_LOGI(TAG, "Driver ILI9341 + XPT2046 créé (CYD %dx%d)", CYD_WIDTH, CYD_HEIGHT);
    return HAL_OK;
}
