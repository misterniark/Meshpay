idf_component_register(
    SRCS "src/wallet.c"
         "src/wallet_lock.c"
         "src/wallet_checkpoint.c"
    INCLUDE_DIRS "include"
    REQUIRES dag transaction crypto
)
