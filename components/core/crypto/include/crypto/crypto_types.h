/**
 * @file crypto_types.h
 * @brief Types fondamentaux pour la cryptographie du système de paiement.
 *
 * Définit les tailles des clés, signatures et hashes utilisés dans tout
 * le système. Ed25519 est utilisé pour les signatures, SHA-256 pour les hashes.
 */

#ifndef CRYPTO_TYPES_H
#define CRYPTO_TYPES_H

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

/** Taille d'une clé publique Ed25519 en octets */
#define CRYPTO_PUBLIC_KEY_SIZE  32

/** Taille d'une clé privée Ed25519 en octets (seed 32 + public 32) */
#define CRYPTO_PRIVATE_KEY_SIZE 64

/** Taille d'une signature Ed25519 en octets */
#define CRYPTO_SIGNATURE_SIZE  64

/** Taille d'un hash SHA-256 en octets */
#define CRYPTO_HASH_SIZE       32

/**
 * @brief Clé publique Ed25519.
 *
 * Utilisée comme identifiant unique de chaque device dans le réseau.
 * C'est aussi l'adresse de destination/source dans les transactions.
 */
typedef struct {
    uint8_t bytes[CRYPTO_PUBLIC_KEY_SIZE];
} public_key_t;

/**
 * @brief Paire de clés Ed25519 (publique + privée).
 *
 * Générée une seule fois au premier démarrage du device,
 * puis stockée dans le NVS chiffré.
 */
typedef struct {
    public_key_t public_key;
    uint8_t private_key[CRYPTO_PRIVATE_KEY_SIZE];
} keypair_t;

/**
 * @brief Signature Ed25519 (64 octets).
 *
 * Chaque transaction est signée par l'émetteur pour garantir
 * l'authenticité et l'intégrité.
 */
typedef struct {
    uint8_t bytes[CRYPTO_SIGNATURE_SIZE];
} signature_t;

/**
 * @brief Hash SHA-256 (32 octets).
 *
 * Utilisé comme identifiant unique des transactions dans le DAG.
 * Le hash est calculé sur le contenu sérialisé de la transaction
 * (tous les champs sauf id et signature).
 */
typedef struct {
    uint8_t bytes[CRYPTO_HASH_SIZE];
} hash_t;

/**
 * @brief Compare deux clés publiques en temps constant.
 *
 * Utilise une boucle XOR pour éviter les timing attacks :
 * le temps d'exécution ne dépend pas de la position de la première
 * différence entre les deux clés, contrairement à memcmp.
 *
 * @param a Première clé publique
 * @param b Deuxième clé publique
 * @return true si les clés sont identiques, false sinon
 */
static inline bool public_key_equal(const public_key_t *a, const public_key_t *b)
{
    volatile uint8_t diff = 0;
    for (size_t i = 0; i < CRYPTO_PUBLIC_KEY_SIZE; i++) {
        diff |= a->bytes[i] ^ b->bytes[i];
    }
    return diff == 0;
}

/**
 * @brief Compare deux hashes en temps constant.
 *
 * Même principe que public_key_equal() : on accumule les différences
 * via XOR pour éviter toute fuite d'information par timing.
 *
 * @param a Premier hash
 * @param b Deuxième hash
 * @return true si les hashes sont identiques, false sinon
 */
static inline bool hash_equal(const hash_t *a, const hash_t *b)
{
    volatile uint8_t diff = 0;
    for (size_t i = 0; i < CRYPTO_HASH_SIZE; i++) {
        diff |= a->bytes[i] ^ b->bytes[i];
    }
    return diff == 0;
}

/**
 * @brief Vérifie si une clé publique est vide (tous les octets à zéro).
 *
 * [F-CR-005] Implémentation en temps constant (accumulation OR),
 * symétrique avec `public_key_equal`. Évite un timing oracle sur le
 * nombre de zéros en tête de clé.
 *
 * @param key La clé publique à vérifier
 * @return true si la clé est entièrement nulle, false sinon
 */
static inline bool public_key_is_zero(const public_key_t *key)
{
    volatile uint8_t acc = 0;
    for (size_t i = 0; i < CRYPTO_PUBLIC_KEY_SIZE; i++) {
        acc |= key->bytes[i];
    }
    return acc == 0;
}

/**
 * @brief Vérifie si un hash est vide (tous les octets à zéro).
 *
 * [F-CR-005] Implémentation en temps constant (accumulation OR), même
 * raison que `public_key_is_zero`. Utilisé pour vérifier les slots
 * parents dans une transaction et les checks post-prune.
 *
 * @param h Le hash à vérifier
 * @return true si le hash est entièrement nul, false sinon
 */
static inline bool hash_is_zero(const hash_t *h)
{
    volatile uint8_t acc = 0;
    for (size_t i = 0; i < CRYPTO_HASH_SIZE; i++) {
        acc |= h->bytes[i];
    }
    return acc == 0;
}

#endif /* CRYPTO_TYPES_H */
