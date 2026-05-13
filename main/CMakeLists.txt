# CMakeLists.txt du composant main
# Orchestration du système de paiement décentralisé offline
#
# ESP32  (CYD)       : core_task + espnow_task + lora_task (noeud complet)
# ESP32-S3 (Waveshare) : core_task uniquement (client-only)
#
# Note : espnow et lora_sync sont toujours dans REQUIRES car le checker
# de dépendances ESP-IDF scanne les #include sans évaluer les #if.
# Ces composants sont compilés mais non utilisés sur ESP32-S3.

idf_component_register(
    SRCS
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
    INCLUDE_DIRS "."
    REQUIRES wallet dag transaction crypto
             comm_protocol espnow lora_sync
             currency time_manager device_hal
             nvs_flash esp_timer esp_wifi
             ui
             debug_console
    PRIV_REQUIRES nvs_sec_provider esp_partition
)
