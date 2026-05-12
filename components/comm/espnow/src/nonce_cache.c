/**
 * @file nonce_cache.c
 * @brief Implementation du cache circulaire de nonces anti-rejeu.
 *
 * Ce module fournit la logique pure du cache de nonces, sans aucun
 * mecanisme de verrouillage. Le verrouillage (portMUX spinlock) est
 * gere par le module appelant (espnow.c).
 *
 * Voir nonce_cache.h pour les decisions de design (capacite, FIFO,
 * fix faux-positif nonce=0).
 */

#include "comm/nonce_cache.h"

/**
 * Reinitialise le cache : aucune entree valide, prochain ecriture
 * a l'index 0. Le contenu de `entries` n'est PAS efface — il sera
 * ecrase au fur et a mesure des ajouts, et seen() ignore les
 * positions au-dela de `filled`.
 */
void nonce_cache_init(nonce_cache_t *cache)
{
    cache->idx    = 0;
    cache->filled = 0;
}

/**
 * Parcourt lineairement les `filled` premieres entrees pour verifier
 * si le nonce est deja present. Complexite O(filled) <= O(SIZE).
 *
 * Cle de la correction du bug [Lot B item 2] : on n'inspecte JAMAIS
 * une position non encore ecrite via nonce_cache_add(). Le nonce 0
 * n'est donc considere "vu" que s'il a ete explicitement ajoute.
 */
bool nonce_cache_seen(const nonce_cache_t *cache, uint32_t nonce)
{
    for (uint16_t i = 0; i < cache->filled; i++) {
        if (cache->entries[i] == nonce) return true;
    }
    return false;
}

/**
 * Ecrit le nonce a la position courante du buffer circulaire,
 * avance l'index modulo la taille, et incremente le compteur
 * d'entrees valides (sature a NONCE_CACHE_SIZE).
 *
 * Apres exactement NONCE_CACHE_SIZE ajouts, `filled` atteint son
 * plafond et y reste : seen() scanne alors l'ensemble du buffer.
 */
void nonce_cache_add(nonce_cache_t *cache, uint32_t nonce)
{
    cache->entries[cache->idx] = nonce;
    cache->idx = (uint16_t)((cache->idx + 1) % NONCE_CACHE_SIZE);
    if (cache->filled < NONCE_CACHE_SIZE) {
        cache->filled++;
    }
}
