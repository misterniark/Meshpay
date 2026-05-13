/**
 * @file espnow_hal_esp32.c
 * @brief Wrapper ESP-IDF pour l'API ESP-NOW.
 *
 * Encapsule les appels esp_now_* dans la vtable espnow_hal_t.
 * Nécessite que le Wi-Fi soit initialisé en mode STA avant usage.
 *
 * Le callback de réception d'ESP-IDF a une signature fixe imposée
 * par le framework. On utilise un contexte statique pour convertir
 * vers notre signature de callback.
 */

#include "comm/espnow_hal.h"
#include "esp_now.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "espnow_hal_esp32";

/*
 * Canal Wi-Fi commun a tous les devices Mesh Pay.
 *
 * ESP-NOW exige que l'emetteur et le recepteur soient sur le meme
 * canal Wi-Fi pour communiquer. En mode STA non-connecte, le canal
 * courant n'est pas garanti d'etre identique entre deux devices
 * (il depend du country code et de l'historique de la radio). Sans
 * fixer un canal explicite, deux Mesh Pay peuvent etre sur des canaux
 * differents et ne jamais se voir.
 *
 * On force le canal 1 (autorise dans toutes les regions, FR/EU/US/...)
 * apres esp_wifi_start() et on configure ce meme canal sur chaque peer
 * ajoute via esp_now_add_peer(). Si un jour la coexistence avec un AP
 * local devient necessaire, ce canal devra etre rendu configurable via
 * Kconfig.
 */
#define ESPNOW_WIFI_CHANNEL 1

/* ================================================================
 * Contexte interne
 * ================================================================ */

typedef struct {
    bool           initialized;
    espnow_rx_cb_t rx_cb;       /* Callback utilisateur */
    void          *rx_user_ctx;  /* Contexte du callback utilisateur */
} esp32_espnow_ctx_t;

static esp32_espnow_ctx_t s_ctx;

/* ================================================================
 * Callback ESP-IDF → notre callback
 * ================================================================ */

/**
 * Callback de réception appelé par le framework ESP-NOW.
 *
 * Convertit la signature ESP-IDF vers notre espnow_rx_cb_t.
 * Note : en ESP-IDF v5.4, la signature du callback de réception
 * inclut esp_now_recv_info_t.
 */
static void esp_now_recv_cb(const esp_now_recv_info_t *recv_info,
                             const uint8_t *data, int data_len)
{
    if (!s_ctx.rx_cb || !recv_info || !data || data_len <= 0) return;

    /* Transmettre au callback utilisateur */
    s_ctx.rx_cb(recv_info->src_addr, data, (size_t)data_len,
                s_ctx.rx_user_ctx);
}

/* ================================================================
 * Implémentation de la vtable
 * ================================================================ */

static hal_err_t esp32_init(void *ctx)
{
    (void)ctx;

    /*
     * Initialiser le Wi-Fi en mode STA (requis pour ESP-NOW).
     * Si le Wi-Fi est déjà initialisé, ces appels sont idempotents.
     */
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    wifi_init_config_t wifi_cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t err = esp_wifi_init(&wifi_cfg);
    if (err != ESP_OK && err != ESP_ERR_WIFI_INIT_STATE) {
        ESP_LOGE(TAG, "esp_wifi_init échoué : 0x%x", err);
        return HAL_FAIL;
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());

    /*
     * Fixer explicitement le canal Wi-Fi apres esp_wifi_start().
     * Sans cet appel, le canal courant en mode STA non-connecte
     * peut diverger entre deux devices et ESP-NOW echoue
     * silencieusement (paquets emis mais jamais recus).
     * WIFI_SECOND_CHAN_NONE = pas de canal secondaire HT40.
     */
    err = esp_wifi_set_channel(ESPNOW_WIFI_CHANNEL, WIFI_SECOND_CHAN_NONE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_channel(%d) echoue : 0x%x",
                 ESPNOW_WIFI_CHANNEL, err);
        return HAL_FAIL;
    }
    ESP_LOGI(TAG, "Canal Wi-Fi force a %d pour ESP-NOW", ESPNOW_WIFI_CHANNEL);

    /* Initialiser ESP-NOW */
    err = esp_now_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_now_init échoué : 0x%x", err);
        return HAL_FAIL;
    }

    /* Enregistrer le callback de réception ESP-IDF */
    ESP_ERROR_CHECK(esp_now_register_recv_cb(esp_now_recv_cb));

    /*
     * Ajouter le peer broadcast pour pouvoir envoyer en broadcast.
     * ESP-NOW exige que le peer soit enregistré avant l'envoi.
     *
     * channel = ESPNOW_WIFI_CHANNEL : on enregistre le peer sur le
     * canal commun. La valeur 0 ("canal courant") fonctionne tant
     * que le canal courant ne change pas, mais reste fragile si une
     * routine future modifie le canal (scan Wi-Fi, AP soft-AP...).
     */
    esp_now_peer_info_t broadcast_peer = {0};
    memset(broadcast_peer.peer_addr, 0xFF, 6);
    broadcast_peer.channel = ESPNOW_WIFI_CHANNEL;
    broadcast_peer.encrypt = false;

    err = esp_now_add_peer(&broadcast_peer);
    if (err != ESP_OK && err != ESP_ERR_ESPNOW_EXIST) {
        ESP_LOGW(TAG, "Ajout peer broadcast : 0x%x", err);
    }

    s_ctx.initialized = true;
    ESP_LOGI(TAG, "ESP-NOW initialisé");
    return HAL_OK;
}

static hal_err_t esp32_deinit(void *ctx)
{
    (void)ctx;
    esp_now_deinit();
    s_ctx.initialized = false;
    return HAL_OK;
}

static hal_err_t esp32_send(const uint8_t *dest_mac, const uint8_t *data,
                             size_t len, void *ctx)
{
    (void)ctx;
    if (!dest_mac || !data || len == 0 || len > 250) {
        return HAL_ERR_INVALID;
    }

    /*
     * Ajouter le peer s'il n'est pas encore enregistré.
     * On ignore l'erreur ESP_ERR_ESPNOW_EXIST si le peer existe déjà.
     */
    esp_now_peer_info_t peer = {0};
    memcpy(peer.peer_addr, dest_mac, 6);
    peer.channel = ESPNOW_WIFI_CHANNEL;
    peer.encrypt = false;

    esp_err_t err = esp_now_add_peer(&peer);
    if (err != ESP_OK && err != ESP_ERR_ESPNOW_EXIST) {
        ESP_LOGW(TAG, "Ajout peer échoué : 0x%x", err);
    }

    err = esp_now_send(dest_mac, data, len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_now_send échoué : 0x%x", err);
        return HAL_ERR_IO;
    }

    return HAL_OK;
}

static hal_err_t esp32_broadcast(const uint8_t *data, size_t len, void *ctx)
{
    uint8_t broadcast_addr[] = ESPNOW_BROADCAST_ADDR;
    return esp32_send(broadcast_addr, data, len, ctx);
}

static hal_err_t esp32_set_rx_callback(espnow_rx_cb_t cb, void *user_ctx,
                                        void *ctx)
{
    (void)ctx;
    s_ctx.rx_cb       = cb;
    s_ctx.rx_user_ctx = user_ctx;
    return HAL_OK;
}

/* ================================================================
 * Factory
 * ================================================================ */

/**
 * Créer une instance ESP-NOW basée sur l'API ESP-IDF.
 *
 * @param hal [out] Vtable à remplir
 * @return HAL_OK
 */
hal_err_t espnow_hal_esp32_create(espnow_hal_t *hal)
{
    if (!hal) return HAL_ERR_INVALID;

    memset(&s_ctx, 0, sizeof(s_ctx));

    hal->init           = esp32_init;
    hal->deinit         = esp32_deinit;
    hal->send           = esp32_send;
    hal->broadcast      = esp32_broadcast;
    hal->set_rx_callback = esp32_set_rx_callback;
    hal->ctx            = &s_ctx;

    return HAL_OK;
}
