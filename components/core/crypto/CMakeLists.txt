set(crypto_srcs
    "src/crypto_init.c"
    "src/crypto_keys.c"
    "src/crypto_sign.c"
    "src/crypto_hash.c"
    # ----- Vendor Monocypher 4.0.2 (API crypto_ed25519_*) -----
    # Depuis Lot E.2 (mai 2026) : mbedTLS ne fournit pas de driver Ed25519.
    # On utilise la lib externe Monocypher (CC-0 / BSD-2) vendoree ici
    # pour generer/signer/verifier les keypairs Ed25519 du protocole.
    # Voir vendor/monocypher/README.md et le Journal des corrections.
    "vendor/monocypher/monocypher.c"
    "vendor/monocypher/monocypher-ed25519.c"
)

# Lot E.1 (mai 2026) : on inclut les tests Unity si le projet en cours
# est `meshpay_test_app`. Detection via `idf_build_get_property` :
# c'est la maniere robuste d'avoir le nom du projet visible dans
# le scope du composant (les variables CACHE CMake ne sont pas
# toujours expandues dans le contexte d'idf_component_register).
#
# Le flag `WHOLE_ARCHIVE` force le linker a garder les TEST_CASE()
# (qui n'ont pas de reference directe depuis app_main).
idf_build_get_property(project_name PROJECT_NAME)
if(project_name STREQUAL "meshpay_test_app")
    list(APPEND crypto_srcs "test/test_crypto.c")
    idf_component_register(
        SRCS ${crypto_srcs}
        INCLUDE_DIRS "include"
        PRIV_INCLUDE_DIRS "vendor/monocypher"
        REQUIRES mbedtls freertos
        PRIV_REQUIRES unity
        WHOLE_ARCHIVE
    )
else()
    idf_component_register(
        SRCS ${crypto_srcs}
        INCLUDE_DIRS "include"
        PRIV_INCLUDE_DIRS "vendor/monocypher"
        REQUIRES mbedtls freertos
    )
endif()
