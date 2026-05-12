/**
 * @file hal_display_jd9853.c
 * @brief Driver JD9853 (SPI LCD) + AXS5106L (I2C tactile capacitif)
 *        pour Waveshare ESP32-S3-Touch-LCD-1.47.
 *
 * Écran 172x320 pixels natif, configuré en mode paysage 320x172.
 * RGB565, rétroéclairage PWM via LEDC.
 *
 * Pinout (verifie contre le code source CircuitPython officiel pour la
 * carte ESP32-S3-Touch-LCD-1.47, source de verite plus fiable que le
 * wiki Waveshare qui s'est revele contradictoire) :
 *   - LCD SPI    : MOSI=39, SCK=38, CS=21, DC=45, RST=40
 *   - Retroeclairage : GPIO 46, actif-HAUT, LEDC PWM
 *   - Touch AXS5106L : SCL=41, SDA=42, RST=47, IRQ=48, I2C addr 0x3B
 *   - Driver controleur : JD9853 (172x320, RGB565, col_start=34)
 *
 * Historique des corrections (Lot E, mai 2026) :
 *   - Code original : 5 pins LCD corrects (MOSI/SCK/CS/DC/BL) mais
 *     **LCD RST = 47** qui est en fait le RST du *touch*, pas du LCD.
 *     Le LCD restait coince dans son etat d'usine (display off / sleep)
 *     → ecran noir total meme avec BL allume. Vraie cause du smoke
 *     test 2026-05-12 echoue.
 *   - Lot E.3 (2026-05-12) : BL 46 -> 48 (faux, base sur le wiki
 *     Waveshare touch-lcd-1.47 qui dit 48 — en realite 48 = touch IRQ).
 *     **Annule au Lot E.5**.
 *   - Lot E.4 (2026-05-12) : tous les pins LCD bascules vers ceux du
 *     SKU 31199 (autre modele Waveshare 1.47 sans touch). Erreur de
 *     diagnostic : la carte est en fait du SKU 31202, le marquage
 *     31199 est trompeur. **Annule au Lot E.5**.
 *   - Lot E.5 (2026-05-12) : restauration du pinout original + vraie
 *     correction LCD RST 47 -> 40, source de verite = CircuitPython
 *     board.c officiel : github.com/adafruit/circuitpython/blob/main/
 *     ports/espressif/boards/waveshare_esp32_s3_touch_lcd_1_47/board.c
 */

#include "hal_display_jd9853.h"

#include <string.h>

#include "driver/spi_master.h"
#include "driver/ledc.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "hal_display_jd9853";

/* ================================================================
 * Pins du Waveshare ESP32-S3-Touch-LCD-1.47
 * ================================================================ */

/* LCD SPI — pinout Waveshare ESP32-S3-Touch-LCD-1.47 (SKU 31202).
 *
 * Source de verite : CircuitPython board.c officiel (verifie runtime sur
 * la carte). Lot E.5 (2026-05-12) annule les changements faux des Lots
 * E.3 et E.4 et applique la *vraie* correction : LCD RST 47 -> 40.
 *
 * Note ESP32-S3 strapping : GPIO 45 est un strapping pin VDD_SPI au reset
 * (etat ignore par l'app apres boot, donc OK comme MOSI).
 */
#define JD9853_PIN_MOSI   39
#define JD9853_PIN_SCK    38
#define JD9853_PIN_CS     21
#define JD9853_PIN_DC     45
#define JD9853_PIN_RST    40   /* correction Lot E.5 : etait 47, qui est le RST du *touch*, pas du LCD */
#define JD9853_PIN_BL     46   /* actif-HAUT, LEDC PWM */

/* Tactile AXS5106L (I2C) — corrections Lot E.6 (2026-05-12) :
 *
 *  - Adresse I2C : 0x3B (code original) -> 0x63 (driver Rust de reference
 *    toto04/axs5106l, teste runtime sur cette carte). L'adresse 0x3B etait
 *    fausse, l'I2C transmit retournait NACK silencieusement.
 *  - RST n'est PLUS partage avec le LCD (le LCD RST est sur GPIO 40 depuis
 *    le Lot E.5). GPIO 47 est dedie au touch et doit etre pulse au boot
 *    pour sortir l'AXS5106L de son etat initial.
 */
#define AXS5106_PIN_SCL   41
#define AXS5106_PIN_SDA   42
#define AXS5106_PIN_RST   47
#define AXS5106_PIN_INT   48
#define AXS5106_I2C_ADDR  0x63   /* etait 0x3B (faux) */

/* Registres AXS5106L (cf. driver Rust toto04/axs5106l) */
#define AXS5106_REG_TOUCH  0x01  /* etait 0x00 (faux) — lit 14 octets */
#define AXS5106_REG_DEV_ID 0x08  /* probe optionnel : byte[0] != 0 */

/* Resolution fixe du Waveshare 1.47" en mode PAYSAGE (rotation 90°).
 * Le panneau physique est 172x320 portrait, mais on applique MADCTL
 * rotation pour obtenir un affichage 320x172 paysage.
 * Les dimensions "natives" (avant rotation) sont utilisees pour
 * la transformation des coordonnees tactiles. */
#define WS147_NATIVE_SHORT  172
#define WS147_NATIVE_LONG   320
#define WS147_WIDTH   320   /* Largeur en mode paysage */
#define WS147_HEIGHT  172   /* Hauteur en mode paysage */

/* Offset RAM du panneau JD9853 172x320 (Lot E.5).
 *
 * Le contrôleur a une VRAM 240x320 mais le panneau physique n'utilise
 * que 172 colonnes (de 34 à 205) en portrait natif. En mode paysage
 * (MADCTL 0x60, swap X/Y), l'offset bascule sur l'axe Y (RASET).
 *
 * Sans cet offset, les pixels sont écrits dans la VRAM invisible et
 * l'image effectivement affichée est du bruit (les premières lignes
 * du panneau lisent une zone de RAM jamais initialisée).
 */
#define WS147_Y_OFFSET  34

/* Fréquence horloge SPI pour le JD9853 */
#define JD9853_SPI_CLOCK_HZ  (40 * 1000 * 1000)

/* Configuration LEDC pour le rétroéclairage */
#define BL_LEDC_TIMER      LEDC_TIMER_0
#define BL_LEDC_CHANNEL    LEDC_CHANNEL_0
#define BL_LEDC_MODE       LEDC_LOW_SPEED_MODE
#define BL_LEDC_FREQ_HZ    5000
#define BL_LEDC_RESOLUTION LEDC_TIMER_8_BIT

/* Taille maximale d'une transaction SPI en octets.
 * On découpe les gros transferts flush en blocs de cette taille.
 * Doit être un multiple de 2 (pixels RGB565 = 2 octets). */
#define SPI_MAX_TRANSFER_SIZE  (32 * 1024)

/* ================================================================
 * Commandes JD9853 (similaire ST7789)
 * ================================================================ */
#define JD9853_CMD_SWRESET  0x01  /* Software reset */
#define JD9853_CMD_SLPOUT   0x11  /* Sortie du mode veille */
#define JD9853_CMD_INVON    0x21  /* Inversion d'affichage ON */
#define JD9853_CMD_DISPON   0x29  /* Affichage ON */
#define JD9853_CMD_CASET    0x2A  /* Définition des colonnes */
#define JD9853_CMD_RASET    0x2B  /* Définition des lignes */
#define JD9853_CMD_RAMWR    0x2C  /* Écriture en mémoire RAM */
#define JD9853_CMD_MADCTL   0x36  /* Contrôle d'adressage mémoire */
#define JD9853_CMD_COLMOD   0x3A  /* Format de couleur des pixels */

/* ================================================================
 * Contexte privé du driver
 * ================================================================ */

/**
 * Structure contenant tous les handles matériels nécessaires au driver.
 * Allouée dynamiquement dans init() et libérée implicitement (pas de deinit pour l'instant).
 */
typedef struct {
    spi_device_handle_t     spi_handle;      /* Handle SPI pour le LCD */
    i2c_master_bus_handle_t i2c_bus_handle;   /* Handle du bus I2C master */
    i2c_master_dev_handle_t i2c_dev_handle;   /* Handle du device AXS5106L sur le bus I2C */
    bool                    initialized;      /* Indique si l'init a réussi */
} jd9853_ctx_t;

/* Contexte statique unique (un seul écran supporté) */
static jd9853_ctx_t s_ctx = {0};

/* ================================================================
 * Fonctions utilitaires SPI pour le LCD
 * ================================================================ */

/**
 * Envoi d'une commande au JD9853 (DC=0 pour commande).
 *
 * @param ctx   Contexte du driver
 * @param cmd   Octet de commande à envoyer
 * @return HAL_OK en cas de succès, HAL_FAIL sinon
 */
static hal_err_t jd9853_send_cmd(jd9853_ctx_t *ctx, uint8_t cmd)
{
    /* DC bas = commande */
    gpio_set_level(JD9853_PIN_DC, 0);

    spi_transaction_t t = {
        .length    = 8,     /* 1 octet = 8 bits */
        .tx_buffer = &cmd,
    };

    esp_err_t ret = spi_device_polling_transmit(ctx->spi_handle, &t);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Erreur envoi commande 0x%02X : %s", cmd, esp_err_to_name(ret));
        return HAL_FAIL;
    }
    return HAL_OK;
}

/**
 * Envoi de données au JD9853 (DC=1 pour données).
 *
 * @param ctx  Contexte du driver
 * @param data Pointeur vers les données à envoyer
 * @param len  Nombre d'octets à envoyer
 * @return HAL_OK en cas de succès, HAL_FAIL sinon
 */
static hal_err_t jd9853_send_data(jd9853_ctx_t *ctx, const uint8_t *data, size_t len)
{
    if (len == 0) {
        return HAL_OK;
    }

    /* DC haut = données */
    gpio_set_level(JD9853_PIN_DC, 1);

    spi_transaction_t t = {
        .length    = len * 8,  /* Longueur en bits */
        .tx_buffer = data,
    };

    esp_err_t ret = spi_device_polling_transmit(ctx->spi_handle, &t);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Erreur envoi données (%zu octets) : %s", len, esp_err_to_name(ret));
        return HAL_FAIL;
    }
    return HAL_OK;
}

/**
 * Envoi d'une commande suivie d'un unique octet de données.
 * Raccourci fréquemment utilisé dans la séquence d'initialisation.
 *
 * @param ctx  Contexte du driver
 * @param cmd  Octet de commande
 * @param data Octet de données
 * @return HAL_OK en cas de succès
 */
static hal_err_t jd9853_send_cmd_data(jd9853_ctx_t *ctx, uint8_t cmd, uint8_t data)
{
    hal_err_t err = jd9853_send_cmd(ctx, cmd);
    if (err != HAL_OK) {
        return err;
    }
    return jd9853_send_data(ctx, &data, 1);
}

/* ================================================================
 * Initialisation matérielle
 * ================================================================ */

/**
 * Configuration des GPIO utilisées par le LCD (DC, RST).
 * Ces broches sont pilotées manuellement (pas via SPI).
 */
static hal_err_t gpio_init(void)
{
    /* Configuration de la broche DC (Data/Command) en sortie */
    gpio_config_t dc_conf = {
        .pin_bit_mask = (1ULL << JD9853_PIN_DC),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    esp_err_t ret = gpio_config(&dc_conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Erreur config GPIO DC : %s", esp_err_to_name(ret));
        return HAL_FAIL;
    }

    /* Configuration de la broche RST LCD (Reset) en sortie */
    gpio_config_t rst_conf = {
        .pin_bit_mask = (1ULL << JD9853_PIN_RST),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    ret = gpio_config(&rst_conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Erreur config GPIO RST LCD : %s", esp_err_to_name(ret));
        return HAL_FAIL;
    }

    /* Configuration de la broche RST Touch (AXS5106L) en sortie (Lot E.6).
     * GPIO 47 doit etre pulse au boot pour sortir l'AXS5106L de son
     * etat initial — sans ca, le touch ne repond pas aux requetes I2C. */
    gpio_config_t rst_touch_conf = {
        .pin_bit_mask = (1ULL << AXS5106_PIN_RST),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    ret = gpio_config(&rst_touch_conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Erreur config GPIO RST Touch : %s", esp_err_to_name(ret));
        return HAL_FAIL;
    }

    return HAL_OK;
}

/**
 * Reset matériel du contrôleur touch AXS5106L via la broche RST (GPIO 47).
 * Séquence (cf. driver Rust toto04/axs5106l, Lot E.6) :
 *   - RST bas pendant 200 ms
 *   - RST haut, puis attente 300 ms pour stabilisation du chip.
 */
static void axs5106_hw_reset(void)
{
    gpio_set_level(AXS5106_PIN_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(200));
    gpio_set_level(AXS5106_PIN_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(300));
}

/**
 * Initialisation du bus SPI2 et ajout du device JD9853.
 *
 * @param ctx Contexte du driver (spi_handle sera rempli)
 * @return HAL_OK en cas de succès
 */
static hal_err_t spi_init(jd9853_ctx_t *ctx)
{
    /* Configuration du bus SPI */
    spi_bus_config_t bus_cfg = {
        .mosi_io_num     = JD9853_PIN_MOSI,
        .miso_io_num     = -1,               /* Pas de MISO, écran en écriture seule */
        .sclk_io_num     = JD9853_PIN_SCK,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = SPI_MAX_TRANSFER_SIZE,
    };

    esp_err_t ret = spi_bus_initialize(SPI2_HOST, &bus_cfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Erreur init bus SPI2 : %s", esp_err_to_name(ret));
        return HAL_FAIL;
    }

    /* Ajout du JD9853 comme device SPI */
    spi_device_interface_config_t dev_cfg = {
        .clock_speed_hz = JD9853_SPI_CLOCK_HZ,
        .mode           = 0,                  /* SPI mode 0 (CPOL=0, CPHA=0) */
        .spics_io_num   = JD9853_PIN_CS,
        .queue_size     = 7,
        .flags          = SPI_DEVICE_NO_DUMMY, /* Pas de cycle dummy en half-duplex */
    };

    ret = spi_bus_add_device(SPI2_HOST, &dev_cfg, &ctx->spi_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Erreur ajout device SPI : %s", esp_err_to_name(ret));
        return HAL_FAIL;
    }

    ESP_LOGI(TAG, "Bus SPI2 initialisé (clock=%d MHz)", JD9853_SPI_CLOCK_HZ / 1000000);
    return HAL_OK;
}

/**
 * Initialisation du bus I2C master et ajout du device AXS5106L.
 *
 * Utilise la nouvelle API i2c_master d'ESP-IDF v5.x avec
 * i2c_master_bus_handle_t et i2c_master_dev_handle_t.
 *
 * @param ctx Contexte du driver (i2c_bus_handle et i2c_dev_handle seront remplis)
 * @return HAL_OK en cas de succès
 */
static hal_err_t i2c_init(jd9853_ctx_t *ctx)
{
    /* Configuration du bus I2C master */
    i2c_master_bus_config_t bus_cfg = {
        .clk_source        = I2C_CLK_SRC_DEFAULT,
        .i2c_port          = I2C_NUM_0,
        .scl_io_num        = AXS5106_PIN_SCL,
        .sda_io_num        = AXS5106_PIN_SDA,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,   /* Pull-up internes pour le tactile */
    };

    esp_err_t ret = i2c_new_master_bus(&bus_cfg, &ctx->i2c_bus_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Erreur init bus I2C master : %s", esp_err_to_name(ret));
        return HAL_FAIL;
    }

    /* Ajout du device AXS5106L sur le bus I2C.
     * Frequence baissee de 400 kHz a 100 kHz (Lot E.6) : le driver
     * i2c_master ESP-IDF v5.4.3 a un bug connu (issue #14030 et autres)
     * ou un NACK initial peut bloquer toutes les transactions suivantes
     * en ESP_ERR_INVALID_STATE, surtout a 400 kHz. 100 kHz (Standard
     * mode) est plus tolerant et largement suffisant pour un touch
     * polle 30x/sec. */
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = AXS5106_I2C_ADDR,
        .scl_speed_hz    = 100000,             /* 100 kHz (Standard mode) */
    };

    ret = i2c_master_bus_add_device(ctx->i2c_bus_handle, &dev_cfg, &ctx->i2c_dev_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Erreur ajout device I2C AXS5106L : %s", esp_err_to_name(ret));
        return HAL_FAIL;
    }

    /* Probe du device a l'adresse declaree (Lot E.6) — confirme que
     * l'AXS5106L repond sur le bus. Diagnostic important pour distinguer
     * une bonne adresse I2C d'une mauvaise (NACK silencieux sinon). */
    esp_err_t probe_ret = i2c_master_probe(ctx->i2c_bus_handle,
                                           AXS5106_I2C_ADDR, 100 /* ms */);
    if (probe_ret == ESP_OK) {
        ESP_LOGI(TAG, "I2C master initialise — AXS5106L @ 0x%02X repond (probe OK)",
                 AXS5106_I2C_ADDR);
    } else {
        ESP_LOGW(TAG, "I2C master initialise — AXS5106L @ 0x%02X NE REPOND PAS (probe %s) ; "
                      "touch indisponible jusqu'a investigation",
                 AXS5106_I2C_ADDR, esp_err_to_name(probe_ret));
    }
    return HAL_OK;
}

/**
 * Initialisation du rétroéclairage via LEDC (PWM).
 * Résolution 8 bits (0-255), fréquence 5 kHz.
 * Le rétroéclairage démarre éteint (duty=0).
 *
 * @return HAL_OK en cas de succès
 */
static hal_err_t backlight_init(void)
{
    /* Configuration du timer LEDC */
    ledc_timer_config_t timer_cfg = {
        .speed_mode      = BL_LEDC_MODE,
        .timer_num       = BL_LEDC_TIMER,
        .duty_resolution = BL_LEDC_RESOLUTION,
        .freq_hz         = BL_LEDC_FREQ_HZ,
        .clk_cfg         = LEDC_AUTO_CLK,
    };

    esp_err_t ret = ledc_timer_config(&timer_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Erreur config timer LEDC : %s", esp_err_to_name(ret));
        return HAL_FAIL;
    }

    /* Configuration du canal LEDC sur la broche du rétroéclairage */
    ledc_channel_config_t channel_cfg = {
        .speed_mode = BL_LEDC_MODE,
        .channel    = BL_LEDC_CHANNEL,
        .timer_sel  = BL_LEDC_TIMER,
        .intr_type  = LEDC_INTR_DISABLE,
        .gpio_num   = JD9853_PIN_BL,
        .duty       = 0,                      /* Éteint au démarrage */
        .hpoint     = 0,
    };

    ret = ledc_channel_config(&channel_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Erreur config canal LEDC : %s", esp_err_to_name(ret));
        return HAL_FAIL;
    }

    ESP_LOGI(TAG, "Rétroéclairage LEDC initialisé (GPIO %d)", JD9853_PIN_BL);
    return HAL_OK;
}

/**
 * Reset matériel du JD9853 via la broche RST.
 * Séquence : RST bas pendant 20 ms, puis haut avec attente de 120 ms
 * pour laisser le contrôleur se stabiliser.
 */
static void jd9853_hw_reset(void)
{
    gpio_set_level(JD9853_PIN_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(JD9853_PIN_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(120));
}

/*
 * Séquence d'initialisation manufacturer du JD9853 (Lot E.5).
 *
 * Reprise depuis le board.c officiel CircuitPython, qui est testée
 * runtime sur cette carte exacte (Waveshare ESP32-S3-Touch-LCD-1.47).
 * Source : github.com/adafruit/circuitpython/.../waveshare_esp32_s3_touch_lcd_1_47/board.c
 *
 * Format de chaque entrée : <cmd_byte>, <num_data | DELAY_FLAG>, <data...>
 *   - Si le bit 7 de num_data est set (= DELAY_FLAG, 0x80), l'octet
 *     qui suit les données est interprété comme un délai en ms.
 *   - num_data = 0 → commande sans paramètre, juste un cmd.
 *
 * Sans cette séquence, les voltage rails internes et le timing du
 * scan ne sont pas calibrés → l'image se forme mais en bruit pixel
 * (rapport utilisateur smoke test 2026-05-12).
 *
 * Note : MADCTL est volontairement laissé à 0x00 ici (portrait
 * natif) et override à 0x60 (paysage) par la fonction d'init après
 * la séquence — c'est pour préserver l'orientation paysage 320×172
 * du projet Mesh Pay sans modifier la table de référence.
 */
#define JD9853_INIT_DELAY_FLAG  0x80

static const uint8_t s_jd9853_init_seq[] = {
    /* SLPOUT + 120 ms */
    0x11, 0x00 | JD9853_INIT_DELAY_FLAG, 120,

    /* Manufacturer registers — power, gamma, timing */
    0xDF, 2, 0x98, 0x53,
    0xB2, 1, 0x23,

    0xB7, 4, 0x00, 0x47, 0x00, 0x6F,
    0xBB, 6, 0x1C, 0x1A, 0x55, 0x73, 0x63, 0xF0,
    0xC0, 2, 0x44, 0xA4,
    0xC1, 1, 0x16,
    0xC3, 8, 0x7D, 0x07, 0x14, 0x06, 0xCF, 0x71, 0x72, 0x77,
    0xC4, 12, 0x00, 0x00, 0xA0, 0x79, 0x0B, 0x0A, 0x16, 0x79, 0x0B, 0x0A, 0x16, 0x82,

    /* Gamma curves (deux moitiés 16 octets chacune) */
    0xC8, 32,
        0x3F, 0x32, 0x29, 0x29, 0x27, 0x2B, 0x27, 0x28,
        0x28, 0x26, 0x25, 0x17, 0x12, 0x0D, 0x04, 0x00,
        0x3F, 0x32, 0x29, 0x29, 0x27, 0x2B, 0x27, 0x28,
        0x28, 0x26, 0x25, 0x17, 0x12, 0x0D, 0x04, 0x00,

    0xD0, 5, 0x04, 0x06, 0x6B, 0x0F, 0x00,
    0xD7, 2, 0x00, 0x30,
    0xE6, 1, 0x14,
    0xDE, 1, 0x01,

    0xB7, 5, 0x03, 0x13, 0xEF, 0x35, 0x35,
    0xC1, 3, 0x14, 0x15, 0xC0,
    0xC2, 2, 0x06, 0x3A,
    0xC4, 2, 0x72, 0x12,
    0xBE, 1, 0x00,
    0xDE, 1, 0x02,

    0xE5, 3, 0x00, 0x02, 0x00,
    0xE5, 3, 0x01, 0x02, 0x00,

    0xDE, 1, 0x00,

    /* TEON (Tearing Effect ON) */
    0x35, 1, 0x00,

    /* COLMOD : RGB565 */
    0x3A, 1, 0x05,

    /* CASET 0x22..0xCD = 34..205 (172 cols) — sera ré-écrit dynamiquement */
    0x2A, 4, 0x00, 0x22, 0x00, 0xCD,

    /* PASET 0x00..0x13F = 0..319 — sera ré-écrit dynamiquement */
    0x2B, 4, 0x00, 0x00, 0x01, 0x3F,

    0xDE, 1, 0x02,
    0xE5, 3, 0x00, 0x02, 0x00,
    0xDE, 1, 0x00,

    /* MADCTL : 0x00 (portrait natif). Sera override en paysage 0x60
     * par la fonction d'init après run_init_sequence. */
    0x36, 1, 0x00,

    /* DISPON */
    0x29, 0,
};

/**
 * Parcourt la table de la séquence d'init et envoie chaque commande
 * au panneau JD9853. Gère le DELAY_FLAG (bit 7 de num_data) pour
 * insérer un délai en ms après la commande.
 */
static hal_err_t jd9853_run_init_sequence(jd9853_ctx_t *ctx,
                                          const uint8_t *seq, size_t seq_len)
{
    size_t i = 0;
    while (i < seq_len) {
        uint8_t cmd       = seq[i++];
        uint8_t num_data  = seq[i++];
        bool    has_delay = (num_data & JD9853_INIT_DELAY_FLAG) != 0;
        uint8_t data_len  = num_data & 0x7F;

        hal_err_t err = jd9853_send_cmd(ctx, cmd);
        if (err != HAL_OK) {
            ESP_LOGE(TAG, "init_seq: send_cmd 0x%02X echoue", cmd);
            return err;
        }

        if (data_len > 0) {
            err = jd9853_send_data(ctx, &seq[i], data_len);
            if (err != HAL_OK) {
                ESP_LOGE(TAG, "init_seq: send_data (cmd 0x%02X, %d octets) echoue",
                         cmd, data_len);
                return err;
            }
            i += data_len;
        }

        if (has_delay) {
            uint8_t delay_ms = seq[i++];
            vTaskDelay(pdMS_TO_TICKS(delay_ms));
        }
    }
    return HAL_OK;
}

/**
 * Séquence d'initialisation des registres du JD9853.
 * Le JD9853 est similaire au ST7789 et utilise les mêmes commandes
 * standard MIPI DCS.
 *
 * Séquence :
 * 1. Reset matériel
 * 2. Software reset + attente 120 ms
 * 3. Sleep out + attente 120 ms
 * 4. COLMOD → RGB565 (0x05)
 * 5. MADCTL → paysage 0x60 (MV=1, MX=1)
 * 6. INVON → inversion nécessaire pour couleurs correctes
 * 7. CASET → colonnes 0..319 (paysage)
 * 8. RASET → lignes 0..171 (paysage)
 * 9. Display ON
 *
 * @param ctx Contexte du driver
 * @return HAL_OK si toute la séquence réussit
 */
static hal_err_t jd9853_init_registers(jd9853_ctx_t *ctx)
{
    hal_err_t err;

    /* 1. Reset matériel */
    jd9853_hw_reset();

    /* 2. Sequence d'init manufacturer complète (Lot E.5).
     *    Reprise du board.c CircuitPython officiel, testee runtime sur
     *    cette carte exacte (Waveshare ESP32-S3-Touch-LCD-1.47). Contient
     *    tous les registres de power, gamma et timing necessaires au
     *    panneau JD9853. Sans cette sequence, les voltage rails internes
     *    sont mal calibres et le scan produit du bruit pixel.
     *    Le tableau s_jd9853_init_seq inclut deja : SLPOUT, registres
     *    manufacturer (0xDF, 0xB2, 0xB7, 0xBB, 0xC0..C4, 0xC8 gamma,
     *    0xD0/D7/E6/DE/E5, BE), TEON, COLMOD, CASET/RASET initiaux,
     *    MADCTL=0x00 (portrait), DISPON. */
    err = jd9853_run_init_sequence(ctx, s_jd9853_init_seq, sizeof(s_jd9853_init_seq));
    if (err != HAL_OK) {
        ESP_LOGE(TAG, "Sequence init JD9853 echouee");
        return err;
    }

    /* 3. Override MADCTL : 0x60 (paysage MV=1, MX=1).
     *    La sequence d'init met MADCTL 0x00 (portrait natif), on
     *    rebascule en paysage 320x172 pour preserver l'orientation
     *    attendue par l'UI Mesh Pay. En paysage swap X/Y, l'offset
     *    RAM 34 bascule du col_start vers row_start — gere dans
     *    jd9853_flush() via WS147_Y_OFFSET. */
    err = jd9853_send_cmd_data(ctx, JD9853_CMD_MADCTL, 0x60);
    if (err != HAL_OK) return err;

    /* CASET/RASET/DISPON sont deja envoyes par la sequence d'init
     * manufacturer ci-dessus. Les fenetres seront re-ecrites
     * dynamiquement a chaque flush LVGL via jd9853_flush(). */
    ESP_LOGI(TAG, "Registres JD9853 initialises (%dx%d paysage, RGB565, Y_offset=%d)",
             WS147_WIDTH, WS147_HEIGHT, WS147_Y_OFFSET);
    return HAL_OK;
}

#if 0  /* === Ancienne fin de jd9853_init_registers (Lot E.5) === ***
        * Conservee en bloc inerte pour reference. Les etapes 7, 8 et 9
        * (CASET, RASET, DISPON) sont desormais incluses dans la sequence
        * d'init manufacturer s_jd9853_init_seq, qui est plus complete
        * (gestion correcte des power rails et timing du JD9853).
        */
    /* 7. Définition de la plage de colonnes : 0 à 319 (paysage) */
    {
        uint8_t caset_data[4] = {
            0x00, 0x00,                          /* Colonne de début (0) */
            (WS147_WIDTH - 1) >> 8,              /* Colonne de fin MSB */
            (WS147_WIDTH - 1) & 0xFF,            /* Colonne de fin LSB (171) */
        };
        err = jd9853_send_cmd(ctx, JD9853_CMD_CASET);
        if (err != HAL_OK) return err;
        err = jd9853_send_data(ctx, caset_data, sizeof(caset_data));
        if (err != HAL_OK) return err;
    }

    /* 8. Définition de la plage de lignes : 0 à 171 (paysage) */
    {
        uint8_t raset_data[4] = {
            0x00, 0x00,                          /* Ligne de début (0) */
            (WS147_HEIGHT - 1) >> 8,             /* Ligne de fin MSB */
            (WS147_HEIGHT - 1) & 0xFF,           /* Ligne de fin LSB (319) */
        };
        err = jd9853_send_cmd(ctx, JD9853_CMD_RASET);
        if (err != HAL_OK) return err;
        err = jd9853_send_data(ctx, raset_data, sizeof(raset_data));
        if (err != HAL_OK) return err;
    }

    /* 9. Activation de l'affichage */
    err = jd9853_send_cmd(ctx, JD9853_CMD_DISPON);
    if (err != HAL_OK) return err;

    ESP_LOGI(TAG, "Registres JD9853 initialisés (%dx%d, RGB565, paysage)",
             WS147_WIDTH, WS147_HEIGHT);
    return HAL_OK;
}
#endif /* Lot E.5 — ancien init minimaliste desactive */

/* ================================================================
 * Implémentation des fonctions de la vtable HAL
 * ================================================================ */

/**
 * Initialisation complète du driver : GPIO, SPI, I2C, LEDC, registres LCD.
 *
 * @param ctx Contexte opaque (non utilisé, le driver utilise s_ctx interne)
 * @return HAL_OK si toute l'initialisation réussit
 */
static hal_err_t jd9853_init(void *ctx)
{
    (void)ctx;
    hal_err_t err;

    if (s_ctx.initialized) {
        ESP_LOGW(TAG, "Driver déjà initialisé, ignoré");
        return HAL_OK;
    }

    ESP_LOGI(TAG, "Initialisation du driver JD9853 + AXS5106L...");

    /* Étape 1 : GPIO (DC et RST) */
    err = gpio_init();
    if (err != HAL_OK) {
        ESP_LOGE(TAG, "Échec init GPIO");
        return err;
    }

    /* Étape 2 : Bus SPI pour le LCD */
    err = spi_init(&s_ctx);
    if (err != HAL_OK) {
        ESP_LOGE(TAG, "Échec init SPI");
        return err;
    }

    /* Etape 3 : Bus I2C pour le tactile (AXS5106L sur SCL=41 / SDA=42).
     * Reactive au Lot E.5 : sur le SKU 31202, ces pins ne sont pas en
     * conflit avec le LCD (qui utilise CS=21, DC=45, RST=40).
     *
     * Pulse RST du touch AVANT init I2C (Lot E.6) : l'AXS5106L doit
     * sortir de son etat initial pour repondre aux requetes. Sans ca,
     * NACK silencieux sur toutes les transactions. */
    axs5106_hw_reset();

    err = i2c_init(&s_ctx);
    if (err != HAL_OK) {
        ESP_LOGE(TAG, "Échec init I2C");
        return err;
    }

    /* Étape 4 : Rétroéclairage PWM */
    err = backlight_init();
    if (err != HAL_OK) {
        ESP_LOGE(TAG, "Échec init rétroéclairage");
        return err;
    }

    /* Étape 5 : Séquence d'initialisation des registres du JD9853 */
    err = jd9853_init_registers(&s_ctx);
    if (err != HAL_OK) {
        ESP_LOGE(TAG, "Échec init registres JD9853");
        return err;
    }

    s_ctx.initialized = true;
    ESP_LOGI(TAG, "Driver JD9853 + AXS5106L initialisé avec succès");
    return HAL_OK;
}

/**
 * Envoi d'une zone de pixels RGB565 à l'écran.
 *
 * Définit la fenêtre d'adressage (CASET + RASET), puis envoie les
 * données pixel via RAMWR. Les transferts de grande taille sont
 * découpés en blocs de SPI_MAX_TRANSFER_SIZE pour respecter les
 * limites du DMA SPI.
 *
 * @param x1    Coordonnée X du coin supérieur gauche
 * @param y1    Coordonnée Y du coin supérieur gauche
 * @param x2    Coordonnée X du coin inférieur droit (inclus)
 * @param y2    Coordonnée Y du coin inférieur droit (inclus)
 * @param color Tableau de pixels RGB565
 * @param ctx   Contexte opaque (non utilisé)
 * @return HAL_OK en cas de succès
 */
static hal_err_t jd9853_flush(uint16_t x1, uint16_t y1,
                               uint16_t x2, uint16_t y2,
                               const uint16_t *color, void *ctx)
{
    (void)ctx;

    if (!s_ctx.initialized) {
        ESP_LOGE(TAG, "flush() appelé avant init()");
        return HAL_FAIL;
    }

    if (!color) {
        return HAL_ERR_INVALID;
    }

    /* Vérification des bornes */
    if (x2 >= WS147_WIDTH || y2 >= WS147_HEIGHT || x1 > x2 || y1 > y2) {
        ESP_LOGE(TAG, "flush() coordonnées invalides : (%d,%d)-(%d,%d)", x1, y1, x2, y2);
        return HAL_ERR_INVALID;
    }

    hal_err_t err;

    /* Définition de la fenêtre de colonnes (CASET) */
    {
        uint8_t caset_data[4] = {
            (uint8_t)(x1 >> 8), (uint8_t)(x1 & 0xFF),
            (uint8_t)(x2 >> 8), (uint8_t)(x2 & 0xFF),
        };
        err = jd9853_send_cmd(&s_ctx, JD9853_CMD_CASET);
        if (err != HAL_OK) return err;
        err = jd9853_send_data(&s_ctx, caset_data, sizeof(caset_data));
        if (err != HAL_OK) return err;
    }

    /* Définition de la fenêtre de lignes (RASET).
     * Application de WS147_Y_OFFSET=34 — le panneau JD9853 172x320
     * a une VRAM 240x320, la zone visible commence a l'offset 34 sur
     * l'axe court. En mode paysage (MADCTL 0x60), cet offset bascule
     * sur l'axe Y / RASET (Lot E.5).
     */
    {
        uint16_t y1_off = y1 + WS147_Y_OFFSET;
        uint16_t y2_off = y2 + WS147_Y_OFFSET;
        uint8_t raset_data[4] = {
            (uint8_t)(y1_off >> 8), (uint8_t)(y1_off & 0xFF),
            (uint8_t)(y2_off >> 8), (uint8_t)(y2_off & 0xFF),
        };
        err = jd9853_send_cmd(&s_ctx, JD9853_CMD_RASET);
        if (err != HAL_OK) return err;
        err = jd9853_send_data(&s_ctx, raset_data, sizeof(raset_data));
        if (err != HAL_OK) return err;
    }

    /* Commande d'écriture mémoire RAM */
    err = jd9853_send_cmd(&s_ctx, JD9853_CMD_RAMWR);
    if (err != HAL_OK) return err;

    /* Calcul de la taille totale des données pixel en octets */
    size_t total_pixels = (size_t)(x2 - x1 + 1) * (size_t)(y2 - y1 + 1);
    size_t total_bytes  = total_pixels * 2;  /* RGB565 = 2 octets/pixel */

    /* Envoi des données pixel par blocs pour ne pas dépasser la taille
     * maximale du transfert DMA SPI */
    const uint8_t *pixel_data = (const uint8_t *)color;
    size_t remaining = total_bytes;

    while (remaining > 0) {
        size_t chunk = (remaining > SPI_MAX_TRANSFER_SIZE)
                       ? SPI_MAX_TRANSFER_SIZE
                       : remaining;

        err = jd9853_send_data(&s_ctx, pixel_data, chunk);
        if (err != HAL_OK) return err;

        pixel_data += chunk;
        remaining  -= chunk;
    }

    return HAL_OK;
}

/**
 * Lecture de l'état tactile du contrôleur AXS5106L via I2C.
 *
 * Protocole AXS5106L (Lot E.6, source : driver Rust toto04/axs5106l) :
 *  - Lire 14 octets à partir du registre 0x01 (AXS5106_REG_TOUCH).
 *  - Octet 0 : header / etat, peu utile pour un seul doigt.
 *  - Octet 1 : nombre de points tactiles actifs (0..N).
 *  - Pour chaque point i (debut a base = 2 + i*6, 6 octets par point) :
 *      - base+0 : status / X_high (bits[3:0])
 *      - base+1 : X_low
 *      - base+2 : status / Y_high (bits[3:0])
 *      - base+3 : Y_low
 *      - base+4..5 : pression / metadata (ignore par ce driver)
 *
 * NB : le code original Mesh Pay utilisait register 0x00 + 6 octets + count
 * a data[0] — toutes ces valeurs etaient fausses, le touch ne repondait
 * jamais (NACK silencieux).
 *
 * @param point [out] Position et état de pression
 * @param ctx   Contexte opaque (non utilisé)
 * @return HAL_OK en cas de succès
 */
static hal_err_t jd9853_touch_read(hal_touch_point_t *point, void *ctx)
{
    (void)ctx;

    if (!point) {
        return HAL_ERR_INVALID;
    }

    /* Valeurs par défaut : pas de toucher */
    point->x       = 0;
    point->y       = 0;
    point->pressed = false;

    if (!s_ctx.initialized) {
        ESP_LOGE(TAG, "touch_read() appelé avant init()");
        return HAL_FAIL;
    }

    /* Registre de départ pour la lecture (corrige Lot E.6 : etait 0x00) */
    uint8_t reg_addr = AXS5106_REG_TOUCH;

    /* Buffer pour les 14 octets de données tactiles (etait 6, Lot E.6) */
    uint8_t touch_data[14] = {0};

    /* Lecture en DEUX transactions I2C separees (Lot E.6, fix bug
     * i2c_master ESP-IDF v5.4 + AXS5106L qui ne gere pas le RESTART
     * condition correctement) :
     *  1. write reg_addr (transaction STOP a la fin)
     *  2. read 14 octets (transaction independante)
     *
     * Le driver Rust toto04/axs5106l de reference fait pareil — c'est
     * un pattern necessaire pour cet AXS5106L. La version combinee
     * write-read (i2c_master_transmit_receive) provoque un
     * ESP_ERR_INVALID_STATE persistant des le 2e appel sur cette
     * combinaison driver/device. */
    esp_err_t ret = i2c_master_transmit(
        s_ctx.i2c_dev_handle,
        &reg_addr, 1,
        100                    /* Timeout en ms */
    );
    if (ret == ESP_OK) {
        ret = i2c_master_receive(
            s_ctx.i2c_dev_handle,
            touch_data, sizeof(touch_data),
            100
        );
    }

    if (ret != ESP_OK) {
        /* Erreur I2C non fatale. Logue en WARN au plus 1x/seconde
         * (Lot E.6 diagnostic). LVGL polle touch_read ~30 fois/s, donc
         * un throttle est obligatoire sinon le log se sature. */
        static int64_t last_warn_us = 0;
        int64_t now_us = esp_timer_get_time();
        if (now_us - last_warn_us > 1000000) {
            ESP_LOGW(TAG, "I2C touch read echec : %s (probe bus / pinout / RST)",
                     esp_err_to_name(ret));
            last_warn_us = now_us;
        }

        /* Recovery sur ESP_ERR_INVALID_STATE (Lot E.6).
         * Bug connu i2c_master ESP-IDF v5.4 (issue #14030) : un NACK
         * initial bloque le driver dans cet etat. `i2c_master_bus_reset`
         * remet le bus a zero (genere des STOP conditions, reset l'etat
         * interne du peripherique I2C). */
        if (ret == ESP_ERR_INVALID_STATE && s_ctx.i2c_bus_handle != NULL) {
            esp_err_t reset_ret = i2c_master_bus_reset(s_ctx.i2c_bus_handle);
            if (reset_ret != ESP_OK) {
                ESP_LOGD(TAG, "i2c_master_bus_reset echec : %s",
                         esp_err_to_name(reset_ret));
            }
        }
        return HAL_OK;
    }

    /* Nombre de points tactiles actifs : octet 1 du buffer AXS5106L */
    uint8_t num_points = touch_data[1];

    /* Trace press/release en DEBUG (silencieuse en INFO).
     * Le static logge aux transitions seulement, pas a chaque polling
     * LVGL (~30 fois/sec). Pour activer en runtime :
     *   esp_log_level_set("hal_display_jd9853", ESP_LOG_DEBUG); */
    static bool was_pressed = false;

    if (num_points == 0) {
        if (was_pressed) {
            ESP_LOGD(TAG, "Touch release");
            was_pressed = false;
        }
        return HAL_OK;
    }

    /* Premier point : base d'extraction = octet 2 (6 octets par point) */
    uint16_t raw_x = ((uint16_t)(touch_data[2] & 0x0F) << 8) | touch_data[3];
    uint16_t raw_y = ((uint16_t)(touch_data[4] & 0x0F) << 8) | touch_data[5];

    if (!was_pressed) {
        ESP_LOGD(TAG, "Touch press : num=%u raw=(%u,%u) header=0x%02X",
                 num_points, raw_x, raw_y, touch_data[0]);
        was_pressed = true;
    }

    /* Transformation portrait → paysage pour correspondre au MADCTL 0x60.
     * Le contrôleur tactile AXS5106L renvoie toujours les coordonnées
     * en orientation native du panneau (portrait 172×320) :
     *   raw_x ∈ [0, 171]  (axe court)
     *   raw_y ∈ [0, 319]  (axe long)
     *
     * Avec MADCTL 0x60 (MV=1 MX=1) cote LCD, l'ecran affiche en paysage
     * 320×172. Empiriquement (Lot E.6, feedback utilisateur smoke test
     * 2026-05-12) : l'axe Y du touch n'est PAS miroir comme suppose au
     * depart — le miroir MX du LCD ne s'applique qu'aux pixels affiches,
     * pas au repere du contrôleur tactile. On enleve donc le miroir.
     *
     * Transformation :
     *   landscape_x = raw_y                    (axe long -> X paysage)
     *   landscape_y = raw_x                    (axe court -> Y paysage, sans miroir)
     */
    uint16_t landscape_x = raw_y;
    uint16_t landscape_y = raw_x;

    /* Clamp aux limites de l'écran paysage */
    if (landscape_x >= WS147_WIDTH) {
        landscape_x = WS147_WIDTH - 1;
    }
    if (landscape_y >= WS147_HEIGHT) {
        landscape_y = WS147_HEIGHT - 1;
    }

    point->x       = landscape_x;
    point->y       = landscape_y;
    point->pressed = true;

    return HAL_OK;
}

/**
 * Réglage de la luminosité du rétroéclairage via PWM LEDC.
 *
 * @param brightness Luminosité de 0 (éteint) à 100 (maximum)
 * @param ctx        Contexte opaque (non utilisé)
 * @return HAL_OK en cas de succès
 */
static hal_err_t jd9853_set_backlight(uint8_t brightness, void *ctx)
{
    (void)ctx;

    /* Clamp de la valeur à 100 maximum */
    if (brightness > 100) {
        brightness = 100;
    }

    /* Conversion de la plage 0-100 vers la plage 0-255 (résolution 8 bits)
     * Formule : duty = brightness * 255 / 100 */
    uint32_t duty = (uint32_t)brightness * 255 / 100;

    esp_err_t ret = ledc_set_duty(BL_LEDC_MODE, BL_LEDC_CHANNEL, duty);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Erreur réglage duty LEDC : %s", esp_err_to_name(ret));
        return HAL_FAIL;
    }

    ret = ledc_update_duty(BL_LEDC_MODE, BL_LEDC_CHANNEL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Erreur mise à jour duty LEDC : %s", esp_err_to_name(ret));
        return HAL_FAIL;
    }

    ESP_LOGD(TAG, "Rétroéclairage réglé à %d%% (duty=%lu)", brightness, (unsigned long)duty);
    return HAL_OK;
}

/**
 * Retourne la résolution fixe de l'écran Waveshare 1.47" (172x320).
 *
 * @param width  [out] Largeur en pixels (172)
 * @param height [out] Hauteur en pixels (320)
 * @param ctx    Contexte opaque (non utilisé)
 * @return HAL_OK en cas de succès
 */
static hal_err_t jd9853_get_resolution(uint16_t *width, uint16_t *height,
                                        void *ctx)
{
    (void)ctx;
    if (width)  *width  = WS147_WIDTH;
    if (height) *height = WS147_HEIGHT;
    return HAL_OK;
}

/* ================================================================
 * Factory
 * ================================================================ */

/**
 * Création d'une instance du driver d'affichage JD9853 + AXS5106L.
 *
 * Remplit la vtable hal_display_t avec les fonctions d'implémentation
 * réelles (SPI LCD, I2C tactile, PWM rétroéclairage).
 *
 * L'initialisation matérielle effective n'a lieu qu'à l'appel de init().
 *
 * @param display [out] Vtable à remplir
 * @return HAL_OK en cas de succès, HAL_ERR_INVALID si display est NULL
 */
hal_err_t hal_display_jd9853_create(hal_display_t *display)
{
    if (!display) {
        return HAL_ERR_INVALID;
    }

    display->init           = jd9853_init;
    display->flush          = jd9853_flush;
    display->touch_read     = jd9853_touch_read;
    display->set_backlight  = jd9853_set_backlight;
    display->get_resolution = jd9853_get_resolution;
    display->ctx            = NULL;   /* Le driver utilise le contexte statique s_ctx */

    ESP_LOGI(TAG, "Driver JD9853 créé (%dx%d + AXS5106L tactile)",
             WS147_WIDTH, WS147_HEIGHT);
    return HAL_OK;
}
