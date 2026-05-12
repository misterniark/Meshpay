# CMakeLists.txt du composant UI
# Interface graphique LVGL : tache de rendu, theme, ecrans, PIN
#
# Utilise LVGL 9.x via le composant manage (idf_component.yml)
# et le HAL display pour le flush/touch.

set(ui_srcs
    "src/ui_task.c"
    "src/ui_manager.c"
    "src/ui_theme.c"
    "src/ui_pin.c"
    "src/ui_pin_numpad.c"
    "src/ui_pin_ledger.c"
    "src/ui_screen_setup.c"
    "src/ui_screen_home.c"
    "src/ui_screen_pay.c"
    "src/ui_screen_history.c"
    "src/ui_screen_settings.c"
    "src/ui_screen_receive.c"
    "src/ui_screen_broadcast.c"
    "src/ui_screen_admin.c"
    "src/ui_screen_mint.c"
    "src/ui_screen_message.c"
    "src/ui_screen_scan.c"
    "src/ui_screen_rename.c"
    "src/ui_screen_forward.c"
)

idf_build_get_property(project_name PROJECT_NAME)
if(project_name STREQUAL "meshpay_test_app")
    list(APPEND ui_srcs
        "test/test_ui_pin.c"
        # TODO Lot E.1bis : test_ui_manager.c definit des stubs
        # `ui_screen_*_handler` qui sont en conflit avec les vrais
        # `src/ui_screen_*.c` du meme composant (multiple definition
        # au linkage). Ce test est concu pour un build isole sans
        # les vrais ecrans — necessite soit un composant test_ui_only
        # separe, soit rendre les vrais handlers __attribute__((weak))
        # pour qu'ils soient surchargeables. Exclu du build pour
        # l'instant : 6 TEST_CASE manquants jusqu'au fix.
        # "test/test_ui_manager.c"
    )
    idf_component_register(
        SRCS ${ui_srcs}
        INCLUDE_DIRS "include"
        REQUIRES lvgl device_hal wallet dag transaction
                 crypto currency comm_protocol
        PRIV_REQUIRES unity
        WHOLE_ARCHIVE
    )
else()
    idf_component_register(
        SRCS ${ui_srcs}
        INCLUDE_DIRS "include"
        REQUIRES lvgl device_hal wallet dag transaction
                 crypto currency comm_protocol
    )
endif()
