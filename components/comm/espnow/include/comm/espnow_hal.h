/**
 * @file espnow_hal.h
 * @brief Abstraction de l'API ESP-NOW pour la testabilité.
 *
 * Vtable similaire à hal_lora_t : encapsule les appels ESP-IDF
 * (esp_now_init, esp_now_send, esp_now_register_recv_cb) derrière
 * une interface à pointeurs de fonctions.
 *
 * Permet de substituer un mock en RAM pour les tests unitaires,
 * sans dépendre du Wi-Fi ou du matériel.
 *
 * Portabilité : ce header utilise hal_types.h pour les codes d'erreur.
 */

#ifndef ESPNOW_HAL_H
#define ESPNOW_HAL_H

#include "hal/hal_types.h"
#include <stdint.h>
#include <stddef.h>

/** Taille d'une adresse MAC ESP-NOW */
#define ESPNOW_MAC_SIZE 6

/** Adresse broadcast ESP-NOW */
#define ESPNOW_BROADCAST_ADDR {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}

/**
 * Callback de réception ESP-NOW.
 *
 * @param src_mac  Adresse MAC de l'émetteur (6 octets)
 * @param data     Données reçues
 * @param len      Taille des données
 * @param user_ctx Contexte utilisateur passé lors de l'enregistrement
 */
typedef void (*espnow_rx_cb_t)(const uint8_t *src_mac,
                                const uint8_t *data, size_t len,
                                void *user_ctx);

/**
 * Vtable d'abstraction ESP-NOW.
 *
 * Chaque pointeur de fonction reçoit le contexte opaque `ctx`.
 */
typedef struct {
    /**
     * Initialiser le sous-système ESP-NOW.
     * Sur ESP32 : init Wi-Fi en mode STA, appeler esp_now_init().
     */
    hal_err_t (*init)(void *ctx);

    /**
     * Arrêter ESP-NOW et libérer les ressources.
     */
    hal_err_t (*deinit)(void *ctx);

    /**
     * Envoyer un paquet unicast à une adresse MAC spécifique.
     *
     * @param dest_mac Adresse MAC destination (6 octets)
     * @param data     Données à envoyer (max COMM_MSG_ESPNOW_MAX octets (321 en V2))
     * @param len      Taille des données
     * @param ctx      Contexte opaque
     */
    hal_err_t (*send)(const uint8_t *dest_mac, const uint8_t *data,
                      size_t len, void *ctx);

    /**
     * Envoyer un paquet en broadcast (FF:FF:FF:FF:FF:FF).
     *
     * @param data Données à envoyer (max COMM_MSG_ESPNOW_MAX octets (321 en V2))
     * @param len  Taille des données
     * @param ctx  Contexte opaque
     */
    hal_err_t (*broadcast)(const uint8_t *data, size_t len, void *ctx);

    /**
     * Enregistrer le callback de réception.
     * Passer NULL pour désactiver.
     *
     * @param cb       Callback de réception (ou NULL)
     * @param user_ctx Contexte utilisateur pour le callback
     * @param ctx      Contexte opaque
     */
    hal_err_t (*set_rx_callback)(espnow_rx_cb_t cb, void *user_ctx,
                                 void *ctx);

    /** Contexte opaque de l'implémentation */
    void *ctx;
} espnow_hal_t;

#endif /* ESPNOW_HAL_H */
