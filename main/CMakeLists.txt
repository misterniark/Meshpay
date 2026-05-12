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
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES wallet dag transaction crypto
             comm_protocol espnow lora_sync
             currency time_manager device_hal
             nvs_flash esp_timer esp_wifi
             ui
    PRIV_REQUIRES nvs_sec_provider esp_partition
)
