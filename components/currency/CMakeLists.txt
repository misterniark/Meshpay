# CMakeLists.txt du composant currency
# Configuration de la monnaie : règles de validation et fonte

set(currency_srcs
    "src/currency_rules.c"
    "src/currency_melt.c"
)

idf_build_get_property(project_name PROJECT_NAME)
if(project_name STREQUAL "meshpay_test_app")
    list(APPEND currency_srcs "test/test_currency.c")
    idf_component_register(
        SRCS ${currency_srcs}
        INCLUDE_DIRS "include"
        REQUIRES crypto transaction
        PRIV_REQUIRES unity
        WHOLE_ARCHIVE
    )
else()
    idf_component_register(
        SRCS ${currency_srcs}
        INCLUDE_DIRS "include"
        REQUIRES crypto transaction
    )
endif()
