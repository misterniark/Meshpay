# CMakeLists.txt du composant HAL
# Couche d'abstraction matérielle : interfaces portables + implémentations par target
#
# ESP32  (CYD)       : ILI9341 display, Wio-E5 LoRa, NVS storage
# ESP32-S3 (Waveshare) : JD9853 display, NVS storage (client-only, pas de LoRa)

set(HAL_SRCS "")

if(CONFIG_IDF_TARGET_ESP32)
    # CYD : display ILI9341 + LoRa Wio-E5 + storage NVS
    list(APPEND HAL_SRCS
        "src/esp32/hal_storage_esp32.c"
        "src/esp32/hal_lora_wio_e5.c"
        "src/esp32/hal_display_ili9341.c"
    )
    set(HAL_PRIV_INCLUDE "src/esp32")
elseif(CONFIG_IDF_TARGET_ESP32S3)
    # Waveshare 1.47" : display JD9853 + storage NVS (pas de LoRa)
    list(APPEND HAL_SRCS
        "src/esp32/hal_storage_esp32.c"
        "src/esp32s3/hal_display_jd9853.c"
    )
    set(HAL_PRIV_INCLUDE "src/esp32s3" "src/esp32")
endif()

idf_component_register(
    SRCS ${HAL_SRCS}
    INCLUDE_DIRS "include"
                 ${HAL_PRIV_INCLUDE}
    REQUIRES nvs_flash driver esp_timer
)
