idf_component_register(
    SRCS "src/tx_create.c"
         "src/tx_serialize.c"
         "src/tx_validate.c"
    INCLUDE_DIRS "include"
    REQUIRES crypto
    PRIV_REQUIRES cbor
)
