# CMakeLists.txt du composant HAL
# Couche d'abstraction matérielle : interfaces portables + implémentations par target
#
# ESP32  (CYD)       : ILI9341 display, Wio-E5 LoRa, NVS storage
# ESP32-S3 (Waveshare) : JD9853 display, Wio-E5 LoRa, NVS storage

# hal_power_stub.c est compilé pour TOUTES les cibles : c'est l'impl
# de production de hal_power tant que le hardware de détection batterie
# n'existe pas (elle renvoie toujours USB). Ce n'est pas un mock de test.
# À déplacer dans les blocs target le jour où une vraie impl GPIO/ADC
# arrive. Voir la spec "13 - Gestion de l'énergie".
set(HAL_SRCS "src/hal_power_stub.c")
set(HAL_PRIV_INCLUDE "")

if(CONFIG_IDF_TARGET_ESP32)
    # CYD : display ILI9341 + LoRa Wio-E5 + storage NVS
    list(APPEND HAL_SRCS
        "src/esp32/hal_storage_esp32.c"
        "src/esp32/hal_lora_wio_e5.c"
        "src/esp32/hal_display_ili9341.c"
    )
    list(APPEND HAL_PRIV_INCLUDE "src/esp32")
elseif(CONFIG_IDF_TARGET_ESP32S3)
    # Waveshare 1.47" : display JD9853 + LoRa Wio-E5 + storage NVS.
    # hal_lora_wio_e5.c est target-agnostic (UART/AT standard) : on
    # reutilise le fichier de src/esp32/ partage avec le CYD.
    list(APPEND HAL_SRCS
        "src/esp32/hal_storage_esp32.c"
        "src/esp32/hal_lora_wio_e5.c"
        "src/esp32s3/hal_display_jd9853.c"
    )
    list(APPEND HAL_PRIV_INCLUDE "src/esp32s3" "src/esp32")
endif()

idf_build_get_property(project_name PROJECT_NAME)
if(project_name STREQUAL "meshpay_test_app")
    # Tests : ajout du mock storage + test_hal_storage.c
    list(APPEND HAL_SRCS
        "src/mock/hal_storage_mock.c"
        "src/mock/hal_display_mock.c"
        "src/mock/hal_lora_mock.c"
        "test/test_hal_storage.c"
        "test/test_hal_power.c"
    )
    list(APPEND HAL_PRIV_INCLUDE "src/mock")
    idf_component_register(
        SRCS ${HAL_SRCS}
        INCLUDE_DIRS "include"
                     ${HAL_PRIV_INCLUDE}
        REQUIRES nvs_flash driver esp_timer
        PRIV_REQUIRES unity
        WHOLE_ARCHIVE
    )
else()
    idf_component_register(
        SRCS ${HAL_SRCS}
        INCLUDE_DIRS "include"
                     ${HAL_PRIV_INCLUDE}
        REQUIRES nvs_flash driver esp_timer
    )
endif()
