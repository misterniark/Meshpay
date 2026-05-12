# CMakeLists.txt du composant comm_protocol
# Types de messages, événements inter-tâches, helpers de packing/unpacking

set(comm_protocol_srcs
    "src/comm_msg.c"
)

idf_build_get_property(project_name PROJECT_NAME)
if(project_name STREQUAL "meshpay_test_app")
    # 8 fichiers de test (broadcast, set_alias, set_beneficiary, ping_pong,
    # announce, ack, attestation, time_sync) = ~96 TEST_CASE au total.
    list(APPEND comm_protocol_srcs
        "test/test_comm_msg_broadcast.c"
        "test/test_comm_msg_set_alias.c"
        "test/test_comm_msg_set_beneficiary.c"
        "test/test_comm_msg_ping_pong.c"
        "test/test_comm_msg_announce.c"
        "test/test_comm_msg_ack.c"
        "test/test_comm_msg_attestation.c"
        "test/test_comm_msg_time_sync.c"
    )
    idf_component_register(
        SRCS ${comm_protocol_srcs}
        INCLUDE_DIRS "include"
        REQUIRES transaction crypto
        PRIV_REQUIRES unity
        WHOLE_ARCHIVE
    )
    # Les helpers `build_signed_*` des tests crypto (Lot B) initialisent
    # les variables `key` / `sig` via des appels de fonction que GCC
    # n'arrive pas a prouver — on tolere le warning maybe-uninitialized
    # dans les tests uniquement (pas dans le code production).
    target_compile_options(${COMPONENT_LIB} PRIVATE
        "-Wno-error=maybe-uninitialized")
else()
    idf_component_register(
        SRCS ${comm_protocol_srcs}
        INCLUDE_DIRS "include"
        REQUIRES transaction crypto
    )
endif()
