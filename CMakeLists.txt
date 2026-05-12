# CMakeLists.txt racine du projet
# Système de paiement décentralisé offline pour ESP32
cmake_minimum_required(VERSION 3.16)

# Inclure les composants du projet
# Chaque sous-dossier de components/core/ est un composant ESP-IDF indépendant
set(EXTRA_COMPONENT_DIRS
    "components/core/crypto"
    "components/core/transaction"
    "components/core/dag"
    "components/core/wallet"
    "components/device_hal"
    "components/comm/comm_protocol"
    "components/comm/espnow"
    "components/comm/lora_sync"
    "components/time_manager"
    "components/currency"
    "components/ui"
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(offline-payment)
