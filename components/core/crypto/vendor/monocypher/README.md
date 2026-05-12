# Monocypher 4.0.2 — vendored

Lib externe Ed25519 (RFC 8032, SHA-512) utilisée par Mesh Pay depuis le Lot E.2
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
| `monocypher-ed25519.c` / `monocypher-ed25519.h` | **Ed25519 standard RFC 8032 (SHA-512)** — c'est ce qu'utilise Mesh Pay |
| `LICENSE.md` | Texte CC-0 + BSD-2 |

## Points d'attention

- **Ne pas confondre `crypto_eddsa_*` (BLAKE2b, non-standard) et `crypto_ed25519_*`
  (SHA-512, standard RFC 8032)**. Mesh Pay utilise exclusivement la variante
  `crypto_ed25519_*` pour rester interopérable.
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
