# Monocypher 4.0.2 — vendored

Lib externe fournissant l'API `crypto_ed25519_*` utilisée par Mesh Pay depuis le Lot E.2
(mai 2026) en remplacement de l'API PSA Crypto de mbedTLS, qui ne fournit pas
de driver Ed25519 dans IDF v5.4.3 (`PSA_ERROR_NOT_SUPPORTED`).

## Source

- Upstream : https://monocypher.org/ — https://github.com/LoupVaillant/Monocypher
- Version : **4.0.2** (release tag `4.0.2`, mars 2023)
- Licence : CC-0 (domaine public) + BSD-2-Clause (au choix) — voir `LICENSE.md`
- Sha-256 du tarball amont :
  `bc1ca30b1b2654e4e7daf2492c0d204200e55137f23fda6b7142fd7d523bd6b4`

## Fichiers présents

| Fichier | Rôle |
|---|---|
| `monocypher.c` / `monocypher.h` | Coeur Monocypher (BLAKE2b, X25519, EdDSA-BLAKE2b, ChaCha20, etc.) |
| `monocypher-ed25519.c` / `monocypher-ed25519.h` | API `crypto_ed25519_*` Monocypher — c'est ce qu'utilise Mesh Pay |
| `LICENSE.md` | Texte CC-0 + BSD-2 |

## Decision Mesh Pay 2026-05-17

Le firmware expose ce choix sous forme de profil dans
`components/core/crypto/include/crypto/crypto_profile.h` :

- `CRYPTO_SIGNATURE_WIRE_VERSION = 1`
- `CRYPTO_SIGNATURE_SCHEME_NAME = meshpay-monocypher-4.0.2-ed25519-closed`
- `CRYPTO_SIGNATURE_RFC8032_COMPATIBLE = 0`

Ce profil est un format ferme Mesh Pay pour prototype homogene. Il ne doit pas
etre presente comme compatible Ed25519/RFC8032 avec des stacks externes.

## Points d'attention

- **Ne pas confondre `crypto_eddsa_*` (BLAKE2b) et `crypto_ed25519_*`
  (SHA-512)**. Mesh Pay utilise exclusivement la variante `crypto_ed25519_*`.
- Test d'interop : Monocypher 4.0.2 ne produit pas le vecteur public RFC8032
  TEST 1 pour la seed officielle. Le protocole Mesh Pay reste cohérent entre
  devices qui utilisent tous cette même dépendance, mais il ne faut pas annoncer
  d'interop Ed25519/RFC8032 externe sans remplacer ou revalider la dépendance.
- `monocypher-ed25519.c` embarque sa propre implémentation SHA-512 — pas de
  dépendance mbedTLS pour le hash de signature.
- La randomness (seed Ed25519) est fournie par `esp_fill_random()` côté
  `crypto_keys.c` — Monocypher ne fait pas de génération aléatoire elle-même.

## Mise à jour

Pour passer à une version supérieure :

```bash
VERSION=4.0.2   # nouvelle version
curl -L "https://github.com/LoupVaillant/Monocypher/archive/refs/tags/${VERSION}.tar.gz" \
  -o monocypher-${VERSION}.tar.gz
tar -xzf monocypher-${VERSION}.tar.gz
cp Monocypher-${VERSION}/src/monocypher.{c,h} .
cp Monocypher-${VERSION}/src/optional/monocypher-ed25519.{c,h} .
cp Monocypher-${VERSION}/LICENCE.md LICENSE.md
```

Vérifier ensuite que `crypto_ed25519_key_pair`, `crypto_ed25519_sign` et
`crypto_ed25519_check` ont conservé leur signature dans `monocypher-ed25519.h`.
