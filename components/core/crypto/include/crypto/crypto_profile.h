/**
 * @file crypto_profile.h
 * @brief Profil cryptographique wire supporte par Mesh Pay.
 *
 * Decision 2026-05-17 :
 * Le prototype utilise Monocypher 4.0.2 via son API `crypto_ed25519_*`.
 * Le comportement est stable entre devices Mesh Pay qui embarquent exactement
 * cette dependance, mais il n'est pas annonce comme interoperable RFC8032.
 *
 * Pour un systeme ouvert/durable, remplacer ce profil par une implementation
 * Ed25519 validee contre les vecteurs RFC8032 officiels, puis incrementer
 * CRYPTO_SIGNATURE_WIRE_VERSION si le format de signature change.
 */

#ifndef CRYPTO_PROFILE_H
#define CRYPTO_PROFILE_H

#define CRYPTO_SIGNATURE_WIRE_VERSION 1
#define CRYPTO_SIGNATURE_SCHEME_NAME "meshpay-monocypher-4.0.2-ed25519-closed"
#define CRYPTO_SIGNATURE_PROVIDER_NAME "Monocypher 4.0.2 crypto_ed25519_*"
#define CRYPTO_SIGNATURE_RFC8032_COMPATIBLE 0

#endif /* CRYPTO_PROFILE_H */
