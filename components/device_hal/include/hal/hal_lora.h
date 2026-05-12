/**
 * @file hal_lora.h
 * @brief Interface abstraite pour la communication LoRa (HAL).
 *
 * Définit une vtable de 5 opérations pour émettre/recevoir des
 * paquets via un module LoRa. L'implémentation concrète gère
 * les spécificités du matériel (AT commands pour Wio-E5, SPI
 * pour SX1276, etc.).
 *
 * La réception est asynchrone : un callback est enregistré via
 * set_rx_callback, puis start_rx active l'écoute continue.
 * L'implémentation appelle le callback depuis son propre contexte
 * (tâche UART, ISR, etc.).
 *
 * Portabilité : ce header n'inclut aucun header spécifique plateforme.
 */

#ifndef HAL_LORA_H
#define HAL_LORA_H

#include "hal/hal_types.h"

/** Taille maximale d'un paquet LoRa en octets */
#define HAL_LORA_MAX_PACKET_SIZE 255

/**
 * Callback de réception LoRa.
 *
 * Appelé par l'implémentation quand un paquet est reçu.
 *
 * @param data     Données reçues
 * @param len      Taille des données en octets
 * @param rssi     RSSI du paquet reçu (dBm)
 * @param user_ctx Contexte utilisateur passé lors de l'enregistrement
 */
typedef void (*hal_lora_rx_cb_t)(const uint8_t *data, size_t len,
                                 int16_t rssi, void *user_ctx);

/**
 * Configuration radio LoRa.
 *
 * Paramètres passés à l'initialisation pour configurer le module.
 * Les valeurs par défaut conviennent pour la plupart des cas d'usage
 * du projet (portée ~2 km en milieu urbain).
 */
typedef struct {
    uint32_t frequency_hz;   /* Fréquence porteuse (ex: 868100000 pour EU868) */
    uint8_t  spreading_factor; /* SF7 à SF12 (défaut SF9 pour bon compromis) */
    uint8_t  bandwidth;      /* 0=125kHz, 1=250kHz, 2=500kHz */
    uint8_t  coding_rate;    /* 1=4/5, 2=4/6, 3=4/7, 4=4/8 */
    int8_t   tx_power_dbm;   /* Puissance d'émission en dBm (max 14 EU) */
} hal_lora_config_t;

/**
 * Vtable de communication LoRa.
 *
 * Chaque pointeur de fonction reçoit le contexte opaque `ctx`.
 */
typedef struct {
    /**
     * Initialiser le module radio avec la configuration donnée.
     * Doit être appelé une seule fois au démarrage.
     *
     * @param config Configuration radio
     * @param ctx    Contexte opaque
     * @return HAL_OK en cas de succès
     */
    hal_err_t (*init)(const hal_lora_config_t *config, void *ctx);

    /**
     * Envoyer un paquet (bloquant).
     * La fonction retourne quand l'envoi est terminé ou en erreur.
     *
     * @param data Données à envoyer
     * @param len  Taille (max HAL_LORA_MAX_PACKET_SIZE)
     * @param ctx  Contexte opaque
     * @return HAL_OK, HAL_ERR_INVALID si len > max, HAL_ERR_IO si échec
     */
    hal_err_t (*send)(const uint8_t *data, size_t len, void *ctx);

    /**
     * Enregistrer le callback de réception.
     * Passer NULL pour désactiver la réception.
     *
     * @param cb       Callback à appeler à chaque paquet reçu (ou NULL)
     * @param user_ctx Contexte utilisateur passé au callback
     * @param ctx      Contexte opaque de l'implémentation
     * @return HAL_OK
     */
    hal_err_t (*set_rx_callback)(hal_lora_rx_cb_t cb, void *user_ctx,
                                 void *ctx);

    /**
     * Activer le mode réception continue.
     * Les paquets reçus sont transmis via le callback enregistré.
     *
     * @param ctx Contexte opaque
     * @return HAL_OK, HAL_ERR_INVALID si aucun callback enregistré
     */
    hal_err_t (*start_rx)(void *ctx);

    /**
     * Mettre le module en veille basse consommation.
     *
     * @param ctx Contexte opaque
     * @return HAL_OK
     */
    hal_err_t (*sleep)(void *ctx);

    /** Contexte opaque de l'implémentation */
    void *ctx;
} hal_lora_t;

#endif /* HAL_LORA_H */
