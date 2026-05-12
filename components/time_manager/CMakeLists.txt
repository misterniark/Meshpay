# CMakeLists.txt du composant time_manager
# Gestion du temps : horloge logique Lamport et synchronisation maître

set(time_manager_srcs
    "src/time_manager.c"
)

idf_build_get_property(project_name PROJECT_NAME)
if(project_name STREQUAL "meshpay_test_app")
    list(APPEND time_manager_srcs "test/test_time_manager.c")
    idf_component_register(
        SRCS ${time_manager_srcs}
        INCLUDE_DIRS "include"
        REQUIRES crypto
        PRIV_REQUIRES unity
        WHOLE_ARCHIVE
    )
else()
    idf_component_register(
        SRCS ${time_manager_srcs}
        INCLUDE_DIRS "include"
        REQUIRES crypto
    )
endif()
