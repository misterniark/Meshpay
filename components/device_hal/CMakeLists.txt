# CMakeLists.txt du composant HAL
# Couche d'abstraction matérielle : interfaces portables + implémentations par target
#
# ESP32  (CYD)         : ILI9341 display, NVS storage
# ESP32-S3 (Waveshare) : JD9853 display, NVS storage
# LoRa : sur toutes les cibles, driver choisi par CONFIG_MESHPAY_LORA_DRIVER.

# hal_power_stub.c est compilé pour TOUTES les cibles : c'est l'impl
# de production de hal_power tant que le hardware de détection batterie
# n'existe pas (elle renvoie toujours USB). Ce n'est pas un mock de test.
# À déplacer dans les blocs target le jour où une vraie impl GPIO/ADC
# arrive. Voir la spec "13 - Gestion de l'énergie".
set(HAL_SRCS "src/hal_power_stub.c")
set(HAL_PRIV_INCLUDE "")

if(CONFIG_IDF_TARGET_ESP32)
    # CYD : display ILI9341 + storage NVS
    list(APPEND HAL_SRCS
        "src/esp32/hal_storage_esp32.c"
        "src/esp32/hal_display_ili9341.c"
    )
elseif(CONFIG_IDF_TARGET_ESP32S3)
    # Waveshare 1.47" : display JD9853 + storage NVS
    list(APPEND HAL_SRCS
        "src/esp32/hal_storage_esp32.c"
        "src/esp32s3/hal_display_jd9853.c"
    )
    list(APPEND HAL_PRIV_INCLUDE "src/esp32s3")
endif()

# --- Driver LoRa : choisi par Kconfig, present sur toutes les cibles ---
# Le LoRa est obligatoire sur tous les devices ; CONFIG_MESHPAY_LORA_DRIVER
# choisit uniquement le module radio. La factory unifiee masque le choix
# au code applicatif.
list(APPEND HAL_SRCS "src/hal_lora_factory.c")
# src/esp32/ contient a la fois du code ESP-IDF partage (hal_storage_esp32.c)
# et les drivers LoRa : ce repertoire d'includes sert les deux.
list(APPEND HAL_PRIV_INCLUDE "src/esp32")
if(CONFIG_MESHPAY_LORA_DRIVER_WIO_E5)
    list(APPEND HAL_SRCS "src/esp32/hal_lora_wio_e5.c")
elseif(CONFIG_MESHPAY_LORA_DRIVER_CORE1262)
    # GLOB est acceptable ici : sx126x/ est un driver vendore stable.
    # Si un fichier est ajoute, lancer idf.py reconfigure manuellement.
    file(GLOB SX126X_SRCS "src/esp32/sx126x/*.c")
    list(APPEND HAL_SRCS
        ${SX126X_SRCS}
        "src/esp32/sx126x_hal.c"
        "src/esp32/core1262_params.c"
        "src/esp32/hal_lora_core1262.c"
    )
    # Le sous-repertoire sx126x/ contient les en-tetes du pilote Semtech
    # (sx126x.h, sx126x_hal.h, sx126x_status.h). Il doit etre dans le
    # chemin d'includes pour que sx126x_hal.c, core1262_params.c et
    # hal_lora_core1262.c puissent les trouver avec #include "sx126x.h".
    list(APPEND HAL_PRIV_INCLUDE "src/esp32/sx126x")
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
    # Test du mapper Core1262 : uniquement quand ce driver est compile.
    if(CONFIG_MESHPAY_LORA_DRIVER_CORE1262)
        list(APPEND HAL_SRCS "test/test_core1262_params.c")
    endif()
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
