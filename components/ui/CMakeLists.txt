# CMakeLists.txt du composant UI
# Interface graphique LVGL : tache de rendu, theme, ecrans, PIN
#
# Utilise LVGL 9.x via le composant manage (idf_component.yml)
# et le HAL display pour le flush/touch.

idf_component_register(
    SRCS "src/ui_task.c"
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
    INCLUDE_DIRS "include"
    REQUIRES lvgl device_hal wallet dag transaction
             crypto currency comm_protocol
)
