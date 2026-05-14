/**
 * @file core1262_params.h
 * @brief Traduction hal_lora_config_t -> parametres radio SX1262.
 *
 * Logique pure (sans dependance ESP-IDF), testable unitairement.
 * Le backend hal_lora_core1262.c consomme le resultat pour configurer
 * le SX1262 via le pilote Semtech.
 */

#ifndef CORE1262_PARAMS_H
#define CORE1262_PARAMS_H

#include <stdbool.h>
#include <stdint.h>

#include "hal/hal_lora.h"
#include "sx126x.h"

/**
 * Parametres radio SX1262 derives d'un hal_lora_config_t.
 *
 * Le champ pkt.pld_len_in_bytes est laisse a 0 : il est renseigne
 * a chaque emission/reception (taille variable du paquet).
 */
typedef struct {
    sx126x_mod_params_lora_t mod;        /**< SF, BW, CR, LDRO */
    sx126x_pkt_params_lora_t pkt;        /**< preambule, header, CRC, IQ */
    uint32_t                 freq_hz;    /**< Frequence porteuse */
    int8_t                   power_dbm;  /**< Puissance d'emission */
} core1262_radio_params_t;

/**
 * Traduit une config HAL portable en parametres SX1262.
 *
 * @param cfg Configuration HAL (freq, SF 7-12, BW 0-2, CR 1-4, puissance)
 * @param out [out] Parametres SX1262 remplis
 * @return true si la config est valide, false si une valeur est hors
 *         plage ou si un pointeur est NULL
 */
bool core1262_map_config(const hal_lora_config_t *cfg,
                         core1262_radio_params_t *out);

#endif /* CORE1262_PARAMS_H */
