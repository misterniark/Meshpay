# CMakeLists.txt du composant main
# Orchestration du système de paiement décentralisé offline
#
# ESP32  (CYD)       : core_task + espnow_task + lora_task (noeud complet)
# ESP32-S3 (Waveshare) : core_task + espnow_task + lora_task (noeud complet,
#                        Wio-E5 cable sur UART1 / GPIO 43-44)
#
# Note : espnow et lora_sync sont toujours dans REQUIRES car le checker
# de dépendances ESP-IDF scanne les #include sans évaluer les #if.

set(meshpay_main_srcs
    "main.c"
    # Modules extraits au Lot D du refactoring (2026-05-13)
    "app_state.c"
    "time_glue.c"
    "stack_monitor.c"
    "peers.c"
    "currency_config_init.c"
    # Persistance NVS (Lot D.2)
    "persistence/nvs_keypair.c"
    "persistence/nvs_checkpoint.c"
    "persistence/nvs_alias.c"
    "persistence/nvs_next_seq.c"
    "persistence/nvs_beneficiary.c"
    # Handlers d'evenements (Lot D.4)
    "handlers/handler_payment.c"
    "handlers/handler_time_sync.c"
    "handlers/handler_broadcast.c"
    "handlers/handler_ping_pong.c"
    "handlers/handler_admin.c"
    # Operations initiees localement (Lot D.5)
    "ops/op_payment.c"
    "ops/op_mint.c"
    "ops/op_beneficiary_forward.c"
    "ops/op_master.c"
    # Core task + UI dispatch + helpers partages (Lot D.6)
    "balance.c"
    "dag_glue.c"
    "core_task.c"
    "ui_dispatch.c"
)

# Lot D.7 : debug console. Reel si CONFIG_MESHPAY_DEBUG_CONSOLE=y, sinon stub.
# Le code applicatif appelle `debug_console_register_dumps()` sans `#if`.
if(CONFIG_MESHPAY_DEBUG_CONSOLE)
    list(APPEND meshpay_main_srcs "debug_console_dumps.c")
else()
    list(APPEND meshpay_main_srcs "debug_console_dumps_stub.c")
endif()

# Lot D.8 : init NVS. Reel chiffre si CONFIG_NVS_ENCRYPTION=y, sinon variante claire.
if(CONFIG_NVS_ENCRYPTION)
    list(APPEND meshpay_main_srcs "app_init/nvs_init_secure.c")
else()
    list(APPEND meshpay_main_srcs "app_init/nvs_init_plain.c")
endif()

# Lot D.3 : facade LoRa. Sur les cibles equipees d'un Wio-E5 (ESP32 CYD
# et ESP32-S3 Waveshare) : impl reelle. Sur d'eventuelles autres cibles :
# stub no-op. Le code applicatif n'a plus aucun `#ifdef MP_HAS_LORA`
# autour des appels.
if(CONFIG_IDF_TARGET_ESP32 OR CONFIG_IDF_TARGET_ESP32S3)
    list(APPEND meshpay_main_srcs "transport/transport_lora.c")
else()
    list(APPEND meshpay_main_srcs "transport/transport_lora_stub.c")
endif()

idf_component_register(
    SRCS ${meshpay_main_srcs}
    INCLUDE_DIRS "."
    REQUIRES wallet dag transaction crypto
             comm_protocol espnow lora_sync
             currency time_manager device_hal
             nvs_flash esp_timer esp_wifi
             ui
             debug_console
    PRIV_REQUIRES nvs_sec_provider esp_partition
)
