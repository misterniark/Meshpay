# CMakeLists.txt du composant lora_sync
# Synchronisation LoRa périodique et fragmentation de paquets.
#
# [Lot C item 7] La dependance directe `dag` a ete retiree : la couche
# LoRa ne connait plus la representation du DAG ni le mutex applicatif
# (inversion de dependance via callback collect_confirmed_txs).
# `transaction` reste necessaire pour le type transaction_t et les
# helpers de (de)serialisation.

idf_component_register(
    SRCS "src/lora_sync.c"
         "src/lora_frag.c"
    INCLUDE_DIRS "include"
    REQUIRES comm_protocol transaction device_hal
)
