# CMakeLists.txt du composant lora_sync
# Synchronisation LoRa périodique et fragmentation de paquets.
#
# [Lot C item 7] La dependance directe `dag` a ete retiree : la couche
# LoRa ne connait plus la representation du DAG ni le mutex applicatif
# (inversion de dependance via callback collect_confirmed_txs).
# `transaction` reste necessaire pour le type transaction_t et les
# helpers de (de)serialisation.

set(lora_sync_srcs
    "src/lora_sync.c"
    "src/lora_frag.c"
    "src/lora_tx_packetize.c"
)

idf_build_get_property(project_name PROJECT_NAME)
if(project_name STREQUAL "meshpay_test_app")
    list(APPEND lora_sync_srcs "test/test_lora_frag.c")
    list(APPEND lora_sync_srcs "test/test_lora_tx_packetize.c")
    list(APPEND lora_sync_srcs "test/test_lora_sync_jitter.c")
    idf_component_register(
        SRCS ${lora_sync_srcs}
        INCLUDE_DIRS "include"
        # `src` reste prive : permet a lora_sync.c et aux tests d'inclure
        # le header local lora_sync_jitter.h sans l'exposer aux autres
        # composants.
        PRIV_INCLUDE_DIRS "src"
        REQUIRES comm_protocol transaction device_hal
        PRIV_REQUIRES unity
        WHOLE_ARCHIVE
    )
    target_compile_options(${COMPONENT_LIB} PRIVATE
        "-Wno-error=maybe-uninitialized")
else()
    idf_component_register(
        SRCS ${lora_sync_srcs}
        INCLUDE_DIRS "include"
        PRIV_INCLUDE_DIRS "src"
        REQUIRES comm_protocol transaction device_hal
    )
endif()
