/**
 * @file currency_melt.h
 * @brief Fonte périodique des soldes (monnaie fondante / démurrage).
 *
 * La fonte réduit automatiquement le solde disponible au fil du temps
 * pour inciter à la dépense. Deux modes de calcul :
 *
 * - BPS (basis points) : pourcentage du solde retiré par tick
 *   Ex: melt_bps = 100 → 1% du solde par tick
 *
 * - FIXED : montant fixe retiré par tick
 *   Ex: melt_fixed_amount = 10 → 10 unités par tick
 *
 * La fonte est un ajustement local du wallet, pas une TX dans le DAG.
 * Tous les devices appliquent la même formule avec la même config →
 * même résultat. Activable uniquement en mode TIME_MODE_MASTER
 * (horloge fiable requise pour que les devices restent synchronisés).
 *
 * Les ticks manqués (device éteint) sont rattrapés au réveil :
 * N ticks consécutifs sont appliqués avant tout nouveau TRANSFER.
 */

#ifndef CURRENCY_MELT_H
#define CURRENCY_MELT_H

#include "currency/currency_config.h"
#include <stdint.h>

/**
 * Nombre maximum de ticks de rattrapage.
 *
 * Borne de sécurité anti-overflow : si un device est éteint très
 * longtemps, on plafonne le nombre de ticks appliqués d'un coup.
 * Avec melt_period = 1 jour, 365 ticks = 1 an de rattrapage.
 */
#define MELT_MAX_CATCHUP_TICKS 365

/**
 * Calculer le nombre de ticks de fonte écoulés depuis le dernier traitement.
 *
 * @param config              Configuration de la monnaie
 * @param last_melt_timestamp Timestamp du dernier traitement de fonte (ms)
 * @param current_time        Temps courant (ms)
 * @return Nombre de ticks à appliquer (0 si fonte désactivée ou pas encore dû)
 */
uint32_t currency_melt_ticks_due(const currency_config_t *config,
                                 uint64_t last_melt_timestamp,
                                 uint64_t current_time);

/**
 * Appliquer la fonte sur un solde pour un nombre de ticks donné.
 *
 * En mode BPS : chaque tick retire melt_bps/10000 du solde courant.
 * Les ticks sont appliqués séquentiellement (intérêt composé).
 *
 * En mode FIXED : chaque tick retire min(melt_fixed_amount, solde).
 *
 * Le solde ne descend jamais en dessous de 0.
 *
 * @param config  Configuration de la monnaie
 * @param balance Solde courant avant fonte
 * @param ticks   Nombre de ticks à appliquer
 * @return Solde après fonte
 */
uint32_t currency_melt_apply(const currency_config_t *config,
                             uint32_t balance,
                             uint32_t ticks);

/**
 * Calculer le nouveau timestamp de référence après application des ticks.
 *
 * Avance last_melt_timestamp de (ticks * melt_period_seconds * 1000) ms.
 * Ne dépasse jamais current_time.
 *
 * @param config              Configuration de la monnaie
 * @param last_melt_timestamp Timestamp du dernier traitement (ms)
 * @param ticks               Nombre de ticks appliqués
 * @param current_time        Temps courant (ms)
 * @return Nouveau timestamp de référence
 */
uint64_t currency_melt_next_timestamp(const currency_config_t *config,
                                      uint64_t last_melt_timestamp,
                                      uint32_t ticks,
                                      uint64_t current_time);

#endif /* CURRENCY_MELT_H */
