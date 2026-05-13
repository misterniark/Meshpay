# CMakeLists.txt du composant debug_console (Mesh Pay)
#
# Strategie de gating :
#   - Le composant est toujours enregistre (le linker scanne les
#     #include avant d'evaluer les conditions, comme pour
#     espnow/lora_sync sur ESP32-S3).
#   - Le contenu de src/debug_console.c est entierement protege par
#     #if CONFIG_MESHPAY_DEBUG_CONSOLE en interne. Quand le flag est
#     a 'n', le fichier objet est vide et l'API publique du header
#     se replie sur des stubs inline qui retournent ESP_OK. Cote
#     binaire : zero octet flash, zero octet RAM, zero surface
#     d'attaque (cf. dette technique — mode Flash Encryption RELEASE).
#
# Le composant ne tire ni dag, ni wallet, ni currency en dependance
# directe : main.c lui fournit des callbacks (pattern Lot C — voir
# composant lora_sync). Les seules dependances sont :
#   - crypto : pour les types hash_t / public_key_t reutilises dans
#              les signatures de callback (API publique).
#   - driver : pour uart_driver_install (utilise dans le .c, prive).
#   - esp_timer : pour les timestamps de debug (utilise dans le .c,
#                 prive).

set(srcs "src/debug_console.c")

# Pattern de test conditionne au test_app (cf. dag/CMakeLists.txt) :
# on n'inclut les fichiers de test Unity que lorsque l'on construit
# le projet meshpay_test_app, sinon Unity n'est pas disponible.
idf_build_get_property(project_name PROJECT_NAME)
if(project_name STREQUAL "meshpay_test_app")
    list(APPEND srcs "test/test_debug_console.c")
    idf_component_register(
        SRCS ${srcs}
        INCLUDE_DIRS "include"
        REQUIRES crypto
        PRIV_REQUIRES driver esp_timer unity
        WHOLE_ARCHIVE
    )
else()
    idf_component_register(
        SRCS ${srcs}
        INCLUDE_DIRS "include"
        REQUIRES crypto
        PRIV_REQUIRES driver esp_timer
    )
endif()
