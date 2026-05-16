/**
 * @file core1262_params.c
 * @brief Implementation du mapper hal_lora_config_t -> SX1262.
 */

#include "core1262_params.h"

/* Plages valides cote HAL. */
#define SF_MIN  7
#define SF_MAX  12
#define CR_MIN  1   /* 1=4/5 */
#define CR_MAX  4   /* 4=4/8 */

/* Plage de puissance du SX1262 (PA haute puissance). */
#define POWER_MIN_DBM  (-9)
#define POWER_MAX_DBM  (22)

bool core1262_map_config(const hal_lora_config_t *cfg,
                         core1262_radio_params_t *out)
{
    if (!cfg || !out) {
        return false;
    }

    /* --- Spreading factor : l'enum SX126X_LORA_SFx vaut numeriquement SFx. --- */
    if (cfg->spreading_factor < SF_MIN || cfg->spreading_factor > SF_MAX) {
        return false;
    }
    out->mod.sf = (sx126x_lora_sf_t)cfg->spreading_factor;

    /* --- Bande passante : 0/1/2 -> 125/250/500 kHz. --- */
    switch (cfg->bandwidth) {
        case 0:  out->mod.bw = SX126X_LORA_BW_125; break;
        case 1:  out->mod.bw = SX126X_LORA_BW_250; break;
        case 2:  out->mod.bw = SX126X_LORA_BW_500; break;
        default: return false;
    }

    /* --- Coding rate : l'enum SX126X_LORA_CR_4_x vaut numeriquement x (1-4). --- */
    if (cfg->coding_rate < CR_MIN || cfg->coding_rate > CR_MAX) {
        return false;
    }
    out->mod.cr = (sx126x_lora_cr_t)cfg->coding_rate;

    /*
     * --- LDRO (Low Data Rate Optimize) ---
     * Obligatoire quand la duree d'un symbole LoRa (2^SF / BW) depasse
     * ~16 ms (Semtech AN). Les combinaisons concernees :
     *   - SF11 ou SF12 en BW125 kHz (cfg->bandwidth == 0)
     *   - SF12 en BW250 kHz         (cfg->bandwidth == 1)
     */
    out->mod.ldro =
        (((cfg->spreading_factor >= 11) && (cfg->bandwidth == 0)) ||
         ((cfg->spreading_factor == 12) && (cfg->bandwidth == 1)))
            ? 1 : 0;

    /* --- Parametres de paquet LoRa : header explicite, CRC actif. --- */
    out->pkt.preamble_len_in_symb = 8;
    out->pkt.header_type          = SX126X_LORA_PKT_EXPLICIT;
    out->pkt.pld_len_in_bytes     = 0;  /* renseigne a chaque TX/RX */
    out->pkt.crc_is_on            = true;
    out->pkt.invert_iq_is_on      = false;

    /* --- Frequence : passe-plat. --- */
    /*
     * [F-HW-009] Validation des bornes du SX1262 (300-960 MHz).
     * Décision design 2026-05-16 : check au niveau HAL pour rejeter
     * les valeurs aberrantes (0, > 1 GHz). La conformité réglementaire
     * (EU868 863-870 MHz, US915, etc.) reste à la charge de la couche
     * applicative.
     */
    if (cfg->frequency_hz < 300000000UL || cfg->frequency_hz > 960000000UL) {
        return false;
    }
    out->freq_hz = cfg->frequency_hz;

    /* --- Puissance : bornee a la plage du SX1262. --- */
    int8_t pwr = cfg->tx_power_dbm;
    if (pwr < POWER_MIN_DBM) pwr = POWER_MIN_DBM;
    if (pwr > POWER_MAX_DBM) pwr = POWER_MAX_DBM;
    out->power_dbm = pwr;

    return true;
}
