set(wallet_srcs
    "src/wallet.c"
    "src/wallet_lock.c"
    "src/wallet_checkpoint.c"
)

idf_build_get_property(project_name PROJECT_NAME)
if(project_name STREQUAL "meshpay_test_app")
    list(APPEND wallet_srcs "test/test_wallet.c")
    idf_component_register(
        SRCS ${wallet_srcs}
        INCLUDE_DIRS "include"
        REQUIRES dag transaction crypto
        # test_wallet.c utilise currency/currency_config.h
        PRIV_REQUIRES unity currency
        WHOLE_ARCHIVE
    )
else()
    idf_component_register(
        SRCS ${wallet_srcs}
        INCLUDE_DIRS "include"
        REQUIRES dag transaction crypto
    )
endif()
