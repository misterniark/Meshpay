set(transaction_srcs
    "src/tx_create.c"
    "src/tx_serialize.c"
    "src/tx_validate.c"
)

# Lot E.1 : si on est dans le test_app, on inclut le test/test_transaction.c
# comme source et on lie unity. WHOLE_ARCHIVE force la conservation des
# TEST_CASE() au linkage (pas de reference directe depuis app_main).
idf_build_get_property(project_name PROJECT_NAME)
if(project_name STREQUAL "meshpay_test_app")
    list(APPEND transaction_srcs "test/test_transaction.c")
    idf_component_register(
        SRCS ${transaction_srcs}
        INCLUDE_DIRS "include"
        REQUIRES crypto
        PRIV_REQUIRES cbor unity
        WHOLE_ARCHIVE
    )
else()
    idf_component_register(
        SRCS ${transaction_srcs}
        INCLUDE_DIRS "include"
        REQUIRES crypto
        PRIV_REQUIRES cbor
    )
endif()
