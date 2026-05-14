# Composant de test pour main/power_manager.c
#
# power_manager.c vit dans main/, que test_app n'inclut pas. Mais
# power_manager.c est dependance-pure : on peut le compiler ici
# directement. Ce composant est auto-decouvert par ESP-IDF car il est
# dans test_app/components/.

idf_component_register(
    SRCS "test_power_manager.c"
         "../../../main/power_manager.c"
    INCLUDE_DIRS "../../../main"
    REQUIRES unity device_hal
    WHOLE_ARCHIVE
)
