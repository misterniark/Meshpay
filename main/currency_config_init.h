/**
 * @file currency_config_init.h
 * @brief Initialisation hardcodee de la configuration de la monnaie.
 *
 * Sera remplacee par un chargement depuis NVS ou une reception via LoRa
 * dans une version future. En attendant, on conserve la config par defaut
 * (TestCoin, 1% de fonte par jour) pour le bring-up.
 *
 * Le nom du fichier est suffixe _init pour eviter la collision avec le
 * composant `currency/currency_config.h` qui definit le type
 * currency_config_t.
 */

#ifndef MESHPAY_CURRENCY_CONFIG_INIT_H
#define MESHPAY_CURRENCY_CONFIG_INIT_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialise s_currency avec la config par defaut (TestCoin).
 *
 * Inclut l'autorite MINT (= cle publique du device). Doit etre appele
 * apres load_or_generate_keypair() pour que s_keypair.public_key soit
 * disponible.
 */
void init_currency_config(void);

#ifdef __cplusplus
}
#endif

#endif /* MESHPAY_CURRENCY_CONFIG_INIT_H */
