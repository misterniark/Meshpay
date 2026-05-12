idf_component_register(
    SRCS "src/crypto_init.c"
         "src/crypto_keys.c"
         "src/crypto_sign.c"
         "src/crypto_hash.c"
         # ----- Vendor Monocypher 4.0.2 (Ed25519 RFC 8032) -----
         # Depuis Lot E.2 (mai 2026) : mbedTLS ne fournit pas de driver Ed25519.
         # On utilise la lib externe Monocypher (CC-0 / BSD-2) vendoree ici
         # pour generer/signer/verifier les keypairs Ed25519 du protocole.
         # Voir vendor/monocypher/README.md et le Journal des corrections.
         "vendor/monocypher/monocypher.c"
         "vendor/monocypher/monocypher-ed25519.c"
    INCLUDE_DIRS "include"
    # Headers de Monocypher accessibles uniquement aux .c du composant crypto
    # (les callers externes n'utilisent que l'API de crypto_keys / crypto_sign).
    PRIV_INCLUDE_DIRS "vendor/monocypher"
    REQUIRES mbedtls freertos
)
