idf_component_register(
    SRCS "test_main.c"
    INCLUDE_DIRS ""
    # Unity fournit le runner ; les autres REQUIRES ci-dessous forcent
    # le linker a inclure les composants de test (qui sont en `WHOLE_ARCHIVE`)
    # meme sans symboles directement reference depuis test_main.c.
    # `crypto` est requis explicitement car test_main.c appelle
    # `crypto_init()` au demarrage.
    REQUIRES unity
             crypto
             transaction
             dag
             wallet
             currency
             time_manager
             comm_protocol
             espnow
             lora_sync
             device_hal
             ui
)
