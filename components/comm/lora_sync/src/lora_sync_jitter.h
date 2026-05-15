/**
 * @file lora_sync_jitter.h
 * @brief Helpers PURS pour le calcul de jitter du cycle de synchronisation
 *        LoRa.
 *
 * Pourquoi un fichier dédié ?
 *   Le cycle `lora_sync_task` dormait exactement `sync_interval_ms`
 *   entre deux émissions. Deux devices bootés en quasi-simultané (ex.
 *   USB hub) entraient donc en cycle au même tick → collision LoRa
 *   systématique (les deux radios en TX au même instant, ni l'une ni
 *   l'autre ne reçoivent l'autre → DAG ne se propage jamais).
 *
 *   Ces deux fonctions calculent le délai à passer à `vTaskDelay` :
 *     1. `lora_jitter_initial_ms()`  → délai aléatoire AVANT le premier
 *        cycle, dans `[0, base_ms]`. Décorrèle l'instant du tout premier
 *        broadcast entre devices co-bootés.
 *     2. `lora_jitter_around_ms()`   → délai aléatoire à chaque cycle,
 *        dans `[base_ms - base_ms*pct/100, base_ms + base_ms*pct/100]`.
 *        Maintient la décorrélation au fil des cycles.
 *
 * Fonctions PURES : la source d'aléa (rnd) est passée en paramètre
 * (typiquement `esp_random()`), pas appelée en interne. Ça rend les
 * fonctions testables unitairement sans dépendance ESP-IDF.
 */
#ifndef LORA_SYNC_JITTER_H
#define LORA_SYNC_JITTER_H

#include <stdint.h>

/**
 * @brief Délai aléatoire de boot, uniformément distribué dans
 *        `[0, base_ms]` inclus.
 *
 * Cas base_ms = 0 : retourne 0 (pas de jitter, comportement défini).
 *
 * @param base_ms Largeur de la fenêtre (ms). 0 autorisé.
 * @param rnd     Mot d'entropie 32 bits (`esp_random()` en runtime).
 * @return        Délai dans `[0, base_ms]`.
 */
static inline uint32_t lora_jitter_initial_ms(uint32_t base_ms, uint32_t rnd)
{
    if (base_ms == 0) return 0;
    /* base_ms + 1 pour rendre la borne haute incluse. La distribution
     * n'est pas parfaitement uniforme (biais modulo) mais négligeable
     * pour notre usage (décorrélation, pas crypto). */
    return rnd % (base_ms + 1U);
}

/**
 * @brief Délai aléatoire autour d'une base, uniformément distribué dans
 *        `[base_ms - delta, base_ms + delta]` où `delta = base_ms * pct / 100`.
 *
 * Saturations :
 *   - base_ms = 0 → retourne 0 (intervalle dégénéré).
 *   - pct >= 100  → clampé à 99 (interdit un délai négatif/nul).
 *   - delta = 0   → retourne base_ms (pas de jitter).
 *
 * @param base_ms Délai nominal (ms).
 * @param pct     Pourcentage de variation (1..99). Saturé si hors plage.
 * @param rnd     Mot d'entropie 32 bits.
 * @return        Délai dans `[base_ms - delta, base_ms + delta]`.
 */
static inline uint32_t lora_jitter_around_ms(uint32_t base_ms,
                                             uint32_t pct,
                                             uint32_t rnd)
{
    if (base_ms == 0) return 0;
    /* Clamp pct : 100% ferait passer le minimum à 0, ce qui spammerait
     * la radio. On garde au moins 1% de marge. */
    if (pct >= 100U) pct = 99U;

    /* delta = base_ms * pct / 100. Sur 32 bits, base_ms <= 2^25 (~33 s)
     * tient sans débordement après multiplication par 99. Pour notre
     * cas (sync_interval_ms = 120000), base_ms * pct max = 11.88M, OK. */
    uint32_t delta = (base_ms * pct) / 100U;
    if (delta == 0) return base_ms;

    /* Fenêtre = 2*delta + 1 (bornes incluses). */
    uint32_t window = (2U * delta) + 1U;
    uint32_t offset = rnd % window;
    return (base_ms - delta) + offset;
}

#endif /* LORA_SYNC_JITTER_H */
