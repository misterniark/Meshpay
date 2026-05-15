/**
 * @file nonce_cache.h
 * @brief Cache circulaire de nonces anti-rejeu.
 *
 * Stocke les N derniers nonces vus pour detecter les messages rejoues.
 * La logique pure (sans verrouillage) est extraite ici pour etre
 * testable independamment. Le verrouillage (portMUX) reste dans
 * le module appelant (espnow.c).
 *
 * Capacite (audit 2026-05-15 [F-EN-003]) :
 *   - NONCE_CACHE_SIZE = 128 entrees (~512 octets RAM).
 *   - Couvre ~2.6 s de memoire au rate-limit global ESP-NOW
 *     (50 msg/s). Augmente depuis 48 entrees (~1 s) pour reduire
 *     la fenetre de rejeu exploitable.
 *   - Historiquement (mai 2026), 64 puis 48 entrees etaient le max
 *     tolere par la marge DRAM. Revalide a 128 apres optimisations
 *     intermediaires. Si un build futur echoue avec linker
 *     "section .dram0.bss will not fit in region dram0_0_seg",
 *     revenir a 64 (encore +33% de couverture vs 48).
 *   - Pour une mitigation plus large (~5 s, 256 entrees, 1 Ko),
 *     liberer de la RAM ailleurs en priorite (buffers LVGL).
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

/** Taille du cache circulaire de nonces. Couvre ~2.6 s a rate-limit max.
 *  [F-EN-003] Augmente de 48 a 128 le 2026-05-15. */
#define NONCE_CACHE_SIZE 128

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
