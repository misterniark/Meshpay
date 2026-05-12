/**
 * @file nonce_cache.h
 * @brief Cache circulaire de nonces anti-rejeu.
 *
 * Stocke les N derniers nonces vus pour detecter les messages rejoues.
 * La logique pure (sans verrouillage) est extraite ici pour etre
 * testable independamment. Le verrouillage (portMUX) reste dans
 * le module appelant (espnow.c).
 *
 * Capacite (audit Lot B, contrainte par la marge RAM disponible) :
 *   - NONCE_CACHE_SIZE = 48 entrees (~192 octets RAM).
 *   - Couvre ~1 s de memoire au rate-limit global ESP-NOW (50 msg/s),
 *     soit ~1.5x l'ancienne taille (32). Les cibles plus larges
 *     (64, 128) faisaient deborder le segment DRAM disponible — la
 *     marge a ete reduite pour rester compatible avec le reste du
 *     firmware (lvgl, wifi, mbedtls).
 *   - Pour une mitigation plus large, il faudra liberer de la RAM
 *     ailleurs (sdkconfig, buffers lvgl) puis remonter cette valeur.
 *
 * Eviction (audit Lot B) :
 *   - Le buffer est circulaire : la plus ancienne entree est ecrasee.
 *   - C'est une eviction FIFO (et non LRU) : l'ordre d'eviction est
 *     l'ordre d'insertion, pas l'ordre de derniere consultation.
 *     Pour notre cas d'usage (anti-rejeu, toutes les entrees ont la
 *     meme valeur de verite), FIFO suffit et est moins couteux.
 *
 * Initialisation (audit Lot B, fix faux-positif sur nonce=0) :
 *   - Avant : memset a zero faisait que nonce_cache_seen(cache, 0)
 *     retournait true pour toutes les positions, donc le nonce 0
 *     etait systematiquement rejete des l'init meme jamais ajoute.
 *   - Maintenant : un compteur `filled` separe distingue les entrees
 *     valides des entrees brutes. seen() ne scanne que les `filled`
 *     premieres positions valides.
 */
#ifndef NONCE_CACHE_H
#define NONCE_CACHE_H

#include <stdint.h>
#include <stdbool.h>

/** Taille du cache circulaire de nonces. Couvre ~1 s a rate-limit max. */
#define NONCE_CACHE_SIZE 48

/** Structure du cache de nonces */
typedef struct {
    uint32_t entries[NONCE_CACHE_SIZE]; /**< Buffer circulaire des nonces */
    uint16_t idx;                       /**< Index de la prochaine ecriture */
    uint16_t filled;                    /**< Nombre d'entrees valides (sature a SIZE) */
} nonce_cache_t;

/**
 * Initialise le cache : index a 0 et `filled` a 0.
 *
 * Le contenu de `entries` est laisse indefini volontairement —
 * la valeur n'est consultee que pour les `filled` premieres positions
 * apres ajout via nonce_cache_add(). Ce design ferme le faux-positif
 * sur nonce=0 qui existait dans l'ancienne version (memset).
 *
 * @param cache Pointeur vers le cache a initialiser
 */
void nonce_cache_init(nonce_cache_t *cache);

/**
 * Verifie si un nonce a deja ete vu dans le cache.
 *
 * Ne scanne que les `filled` premieres positions valides ; les
 * positions au-dela sont considerees non initialisees.
 *
 * Complexite O(filled) — au plus O(NONCE_CACHE_SIZE).
 *
 * @param cache Pointeur vers le cache (lecture seule)
 * @param nonce Le nonce a verifier
 * @return true si le nonce est dans le cache (message rejoue)
 */
bool nonce_cache_seen(const nonce_cache_t *cache, uint32_t nonce);

/**
 * Ajoute un nonce au cache. L'entree la plus ancienne est ecrasee
 * lorsque le buffer circulaire a fait un tour complet (eviction FIFO).
 *
 * Incremente `filled` jusqu'a saturation a NONCE_CACHE_SIZE.
 *
 * @param cache Pointeur vers le cache
 * @param nonce Le nonce a enregistrer
 */
void nonce_cache_add(nonce_cache_t *cache, uint32_t nonce);

#endif /* NONCE_CACHE_H */
