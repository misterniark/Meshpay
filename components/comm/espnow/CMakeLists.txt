# CMakeLists.txt du composant espnow
# Protocole ESP-NOW : découverte de peers et paiement peer-to-peer

idf_component_register(
    SRCS "src/espnow.c"
         "src/espnow_hal_esp32.c"
         "src/nonce_cache.c"
    INCLUDE_DIRS "include"
                 "mock"
    REQUIRES comm_protocol transaction crypto device_hal
    PRIV_REQUIRES esp_wifi esp_event esp_netif nvs_flash esp_timer
)
