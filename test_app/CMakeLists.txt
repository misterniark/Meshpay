# =============================================================================
# Mesh Pay — Test App (Lot E.1, mai 2026)
#
# Projet ESP-IDF separe du firmware principal, dedie a l'execution des tests
# unitaires Unity sur cible. Reutilise tous les composants de ../components/.
#
# Build :   idf.py -C test_app build
# Flash :   idf.py -C test_app -p /dev/cu.usbmodem101 flash monitor
#
# Au boot, un menu interactif Unity (`unity_run_menu()`) s'affiche sur la
# console serie pour selectionner les tests a executer.
# =============================================================================

cmake_minimum_required(VERSION 3.16)

# L'inclusion des tests Unity dans chaque composant est conditionnee
# par le nom du projet (`meshpay_test_app`) detecte cote composant via
# `idf_build_get_property(PROJECT_NAME)`. Aucune variable a positionner
# ici — il suffit que ce projet conserve son nom unique.
#
# Reutilise tous les composants du firmware principal en pointant vers
# leurs dossiers parents : `EXTRA_COMPONENT_DIRS`.
set(EXTRA_COMPONENT_DIRS
    "../components/core/crypto"
    "../components/core/transaction"
    "../components/core/dag"
    "../components/core/wallet"
    "../components/core/tx_lifecycle"
    "../components/device_hal"
    "../components/comm/comm_protocol"
    "../components/comm/espnow"
    "../components/comm/lora_sync"
    "../components/time_manager"
    "../components/currency"
    "../components/ui"
    "../components/debug_console"
    "components/test_power_manager"
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(meshpay_test_app)
