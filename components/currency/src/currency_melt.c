/**
 * @file currency_melt.c
 * @brief Implémentation de la fonte périodique des soldes.
 *
 * La fonte s'applique en ticks discrets. Chaque tick retire un
 * montant du solde selon le mode configuré :
 *
 * - BPS : solde = solde - (solde * melt_bps / 10000)
 *   Les ticks sont appliqués séquentiellement (composé).
 *   Ex: 3 ticks à 1% → solde * 0.99 * 0.99 * 0.99
 *
 * - FIXED : solde = solde - min(melt_fixed_amount, solde)
 *   Chaque tick retire le même montant fixe.
 *
 * Le calcul utilise des entiers 64 bits pour éviter les overflows
 * intermédiaires, mais le résultat est plafonné à uint32.
 */

#include "currency/currency_melt.h"

uint32_t currency_melt_ticks_due(const currency_config_t *config,
                                 uint64_t last_melt_timestamp,
                                 uint64_t current_time)
{
    if (!config || !config->melt_enabled || config->melt_period_seconds == 0) {
        return 0;
    }

    /* Pas de fonte si le temps courant est antérieur au dernier traitement */
    if (current_time <= last_melt_timestamp) {
        return 0;
    }

    /* Calculer le temps écoulé en secondes */
    uint64_t elapsed_ms = current_time - last_melt_timestamp;
    uint64_t elapsed_s  = elapsed_ms / 1000;

    /* Nombre de ticks complets */
    uint64_t ticks = elapsed_s / config->melt_period_seconds;

    /* Plafonner pour éviter un rattrapage trop long */
    if (ticks > MELT_MAX_CATCHUP_TICKS) {
        ticks = MELT_MAX_CATCHUP_TICKS;
    }

    return (uint32_t)ticks;
}

uint32_t currency_melt_apply(const currency_config_t *config,
                             uint32_t balance,
                             uint32_t ticks)
{
    if (!config || !config->melt_enabled || ticks == 0 || balance == 0) {
        return balance;
    }

    /*
     * [H5] Garde pour melt_bps == 0 : pas de fonte, retourner le solde
     * tel quel sans entrer dans la boucle.
     */
    if (config->melt_volume_mode == MELT_MODE_BPS && config->melt_bps == 0) {
        return balance;
    }

    /*
     * [H5] Garde pour melt_bps > CURRENCY_BPS_SCALE : valeur invalide qui
     * provoquerait un underflow sur le calcul (BASE - melt_bps). On retourne
     * 0 car une fonte supérieure à 100% signifie que tout le solde est fondu.
     */
    if (config->melt_volume_mode == MELT_MODE_BPS &&
        config->melt_bps > CURRENCY_BPS_SCALE) {
        return 0;
    }

    uint64_t current = (uint64_t)balance;

    if (config->melt_volume_mode == MELT_MODE_BPS) {
        /*
         * Mode BPS : appliquer les ticks séquentiellement.
         * Chaque tick : current = current - (current * bps / SCALE)
         *             = current * (SCALE - bps) / SCALE
         *
         * On boucle pour chaque tick (composé). Le plafond de
         * MELT_MAX_CATCHUP_TICKS (365) garantit que cette boucle
         * reste raisonnable.
         */
        uint32_t factor = CURRENCY_BPS_SCALE - config->melt_bps;
        for (uint32_t i = 0; i < ticks && current > 0; i++) {
            current = (current * factor) / CURRENCY_BPS_SCALE;
        }
    } else {
        /*
         * Mode FIXED : retirer melt_fixed_amount par tick.
         * On peut optimiser : si ticks * fixed >= current, résultat = 0.
         */
        uint64_t total_melt = (uint64_t)config->melt_fixed_amount * ticks;
        if (total_melt >= current) {
            current = 0;
        } else {
            current -= total_melt;
        }
    }

    return (uint32_t)current;
}

uint64_t currency_melt_next_timestamp(const currency_config_t *config,
                                      uint64_t last_melt_timestamp,
                                      uint32_t ticks,
                                      uint64_t current_time)
{
    /*
     * [F-CU-005] Garde sur melt_period_seconds == 0 ajoutée pour symétrie
     * avec currency_melt_ticks_due. Sans cette garde, un appel direct à
     * currency_melt_next_timestamp avec une config malformée ferait
     * advance = 0 et le timestamp ne progresserait jamais — la fonte
     * boucle indéfiniment au prochain checkpoint.
     */
    if (!config || ticks == 0 || config->melt_period_seconds == 0) {
        return last_melt_timestamp;
    }

    /*
     * [F-CU-003] L'appelant doit garantir current_time monotone non
     * décroissant par rapport à last_melt_timestamp. Sans cet invariant,
     * le plafonnement `new_ts = current_time` ancre la référence sur
     * une horloge régressée et provoque des ticks en double au prochain
     * cycle. dag_glue.c capture `now` une seule fois et le transmet à
     * ticks_due puis à next_timestamp, donc l'invariant est respecté.
     *
     * Avancer le timestamp de référence du nombre exact de ticks appliqués.
     * On avance de ticks * period pour que le prochain calcul de ticks_due
     * soit correct (le reste du temps écoulé sera comptabilisé au prochain
     * appel).
     */
    uint64_t advance = (uint64_t)ticks * (uint64_t)config->melt_period_seconds * 1000ULL;
    uint64_t new_ts = last_melt_timestamp + advance;

    /* Ne pas dépasser le temps courant (sécurité) */
    if (new_ts > current_time) {
        new_ts = current_time;
    }

    return new_ts;
}
