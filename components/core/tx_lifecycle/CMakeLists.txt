set(tx_lifecycle_srcs
    "src/tx_lifecycle.c"
)

idf_build_get_property(project_name PROJECT_NAME)
if(project_name STREQUAL "meshpay_test_app")
    list(APPEND tx_lifecycle_srcs "test/test_tx_lifecycle_chaos.c")
    idf_component_register(
        SRCS ${tx_lifecycle_srcs}
        INCLUDE_DIRS "include"
        REQUIRES dag wallet transaction crypto comm_protocol
        PRIV_REQUIRES unity
        WHOLE_ARCHIVE
    )
else()
    idf_component_register(
        SRCS ${tx_lifecycle_srcs}
        INCLUDE_DIRS "include"
        REQUIRES dag wallet transaction crypto comm_protocol
    )
endif()
