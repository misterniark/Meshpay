/**
 * @file time_glue.h
 * @brief Adaptateurs entre les composants applicatifs et la source de temps.
 *
 * Centralise les wrappers qui faisaient l'interface entre :
 *   - esp_timer_get_time() (source matérielle micro-seconde)
 *   - time_manager (logique Lamport / master-corrected)
 *   - wallet / lock_table / lora_sync (consommateurs)
 *
 * Pourquoi extraire :
 *   Ces fonctions etaient dans main.c (Lot D 2026-05-13). Les sortir
 *   permet au handler du time_sync (Lot 4) et aux ops maitre (Lot 5)
 *   de les utiliser sans dependre du fichier principal.
 *
 * Toutes ces fonctions sont thread-safe (lectures atomiques ou
 * deja protegees par time_manager).
 */

#ifndef MESHPAY_TIME_GLUE_H
#define MESHPAY_TIME_GLUE_H

#include <stdint.h>
#include "transaction/tx_types.h"
#include "app_state.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Retourne le temps monotonique en millisecondes.
 *
 * Wrapper autour de esp_timer_get_time() (us -> ms). Utilisee comme
 * source de temps pour le time_manager, le wallet et la lock_table.
 */
uint64_t platform_get_monotonic_ms(void);

/**
 * @brief Delegue au time_manager pour obtenir le temps monotonique.
 *
 * Utilisee par le wallet et la lock_table (callbacks `get_time_ms`).
 */
uint64_t get_time_ms_wrapper(void);

/**
 * @brief Delegue au time_manager pour le timestamp d'une TX sortante
 *        (Lamport ou master-corrected selon le mode).
 */
uint64_t get_tx_timestamp_wrapper(void);

/*
 * get_lamport_wrapper et main_collect_confirmed_txs sont desormais prives
 * a transport/transport_lora.c (Lot D.3) — ils servaient uniquement a
 * lora_sync_task, qui est cree par la facade LoRa.
 */

/**
 * @brief Applique la fonte en attente sur un solde (mutation wallet).
 *
 * Calcule les ticks ecoules depuis le dernier traitement de fonte,
 * applique la reduction, met a jour s_wallet.last_melt_timestamp.
 * Ne fait rien si la fonte est desactivee ou si le mode TIME_MODE_MASTER
 * n'est pas actif.
 *
 * DOIT etre appele sous s_state_mutex.
 *
 * @param balance [in,out] Solde a ajuster
 * @return Nombre de ticks appliques (0 si aucun)
 */
uint32_t apply_pending_melt(uint32_t *balance);

/**
 * @brief Calcule le solde apres fonte sans modifier l'etat du wallet.
 *
 * Utilisee pour l'affichage UI (lecture seule).
 */
uint32_t compute_melted_balance(uint32_t balance);

#ifdef __cplusplus
}
#endif

#endif /* MESHPAY_TIME_GLUE_H */
