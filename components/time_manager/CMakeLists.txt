# CMakeLists.txt du composant time_manager
# Gestion du temps : horloge logique Lamport et synchronisation maître

idf_component_register(
    SRCS "src/time_manager.c"
    INCLUDE_DIRS "include"
    REQUIRES crypto
)
