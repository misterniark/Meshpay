# CMakeLists.txt du composant espnow
# Protocole ESP-NOW : découverte de peers et paiement peer-to-peer

set(espnow_srcs
    "src/espnow.c"
    "src/espnow_hal_esp32.c"
    "src/nonce_cache.c"
)

idf_build_get_property(project_name PROJECT_NAME)
if(project_name STREQUAL "meshpay_test_app")
    list(APPEND espnow_srcs "test/test_nonce_cache.c")
    idf_component_register(
        SRCS ${espnow_srcs}
        INCLUDE_DIRS "include"
                     "mock"
        REQUIRES comm_protocol transaction crypto device_hal
        PRIV_REQUIRES esp_wifi esp_event esp_netif nvs_flash esp_timer unity
        WHOLE_ARCHIVE
    )
else()
    idf_component_register(
        SRCS ${espnow_srcs}
        INCLUDE_DIRS "include"
                     "mock"
        REQUIRES comm_protocol transaction crypto device_hal
        PRIV_REQUIRES esp_wifi esp_event esp_netif nvs_flash esp_timer
    )
endif()
