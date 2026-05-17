# Profil de signature Mesh Pay — decision 2026-05-17

## Decision

Le firmware actuel utilise le profil:

- `CRYPTO_SIGNATURE_WIRE_VERSION = 1`
- `CRYPTO_SIGNATURE_SCHEME_NAME = meshpay-monocypher-4.0.2-ed25519-closed`
- provider: `Monocypher 4.0.2 crypto_ed25519_*`
- taille pubkey: 32 octets
- taille private key stockee: 64 octets, `seed[32] || public[32]`
- taille signature: 64 octets
- compatibilite RFC8032 annoncee: **non**

Ce profil est acceptable pour le prototype ferme actuel, a condition que tous
les devices Mesh Pay utilisent exactement la meme dependance vendorée.

## Pourquoi ce n'est pas annonce RFC8032

Le test de regression `monocypher_402_seed_to_public_key_regression` verrouille
la sortie observee avec Monocypher 4.0.2 officiel pour la seed du vecteur
RFC8032 TEST 1. Cette sortie ne correspond pas au vecteur public RFC8032.

Conclusion: le systeme peut verifier ses propres signatures entre devices
homogenes, mais il ne doit pas promettre une interop Ed25519/RFC8032 externe
sans migration ou revalidation crypto.

## Regles de documentation produit

Formulation autorisee:

- "Signatures Mesh Pay 64 octets, profil Monocypher 4.0.2 ferme"
- "Identite par cle publique 32 octets"
- "Interop externe Ed25519/RFC8032 non garantie dans le prototype actuel"

Formulation interdite pour l'etat actuel:

- "Ed25519 standard interoperable"
- "compatible RFC8032"
- "ouvrable avec n'importe quelle librairie Ed25519"

## Migration vers un systeme ouvert

Pour un systeme durable ou interoperable:

1. choisir une implementation Ed25519 maintenue et testee contre les vecteurs
   RFC8032 officiels;
2. ajouter des tests de keygen/sign/verify avec vecteurs officiels;
3. documenter le format de cle privee stockee et le format wire exact;
4. incrementer `CRYPTO_SIGNATURE_WIRE_VERSION` si les signatures ou cles
   existantes ne sont plus compatibles;
5. prevoir une migration NVS des keypairs ou une rotation d'identite par device;
6. refuser en runtime les messages d'un profil non supporte si plusieurs profils
   coexistent un jour.

## Invariants testes

- `crypto_profile_monocypher_closed_not_rfc8032` verifie que le code expose le
  profil ferme et `CRYPTO_SIGNATURE_RFC8032_COMPATIBLE == 0`.
- `monocypher_402_seed_to_public_key_regression` verifie le comportement exact
  de la dependance vendorée.
- `sign_et_verify_succes` et les tests de rejet garantissent la coherence
  interne du profil utilise par les devices.
