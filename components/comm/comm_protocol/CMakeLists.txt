# CMakeLists.txt du composant comm_protocol
# Types de messages, événements inter-tâches, helpers de packing/unpacking

idf_component_register(
    SRCS "src/comm_msg.c"
    INCLUDE_DIRS "include"
    REQUIRES transaction crypto
)
