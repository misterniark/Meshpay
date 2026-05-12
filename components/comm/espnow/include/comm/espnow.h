/**
 * @file espnow.h
 * @brief Module ESP-NOW : découverte de peers et protocole de paiement.
 *
 * Ce module implémente deux protocoles au-dessus d'ESP-NOW :
 *
 * 1. Découverte (DISCOVER / ANNOUNCE) :
 *    - Broadcast d'un message DISCOVER avec la clé publique
 *    - Collecte des réponses ANNOUNCE pendant 5 secondes
 *    - L'UI affiche les peers trouvés pour sélection
 *
 * 2. Paiement (TX_LOCKED / TX_ACK) :
 *    - Envoi d'une TX verrouillée au peer sélectionné
 *    - Attente d'un ACK pendant 30 secondes max
 *    - Si ACK reçu → CONFIRMED, sinon → CANCELLED (timeout)
 *
 * Architecture :
 * - espnow_task() est la boucle principale FreeRTOS
 * - Les commandes arrivent via espnow_cmd_queue (de core/UI)
 * - Les événements partent via comm_evt_queue (vers core_task)
 * - Le module ne modifie JAMAIS le DAG ou le wallet directement
 */

#ifndef ESPNOW_H
#define ESPNOW_H

#include "comm/espnow_hal.h"
#include "comm/comm_event.h"
#include "crypto/crypto_types.h"
#include "crypto/crypto_keys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

/** Timeout de découverte en millisecondes */
#define ESPNOW_DISCOVER_TIMEOUT_MS 5000

/** Timeout d'attente d'ACK en millisecondes */
#define ESPNOW_ACK_TIMEOUT_MS      30000

/**
 * Configuration du module ESP-NOW.
 *
 * Passée à espnow_task() comme paramètre pvParameters.
 */
typedef struct {
    espnow_hal_t      *hal;           /* Vtable ESP-NOW (réel ou mock) */
    QueueHandle_t      evt_queue;     /* Queue comm → core_task */
    QueueHandle_t      cmd_queue;     /* Queue core/UI → espnow_task */
    public_key_t       own_pubkey;    /* Clé publique de ce device */
    const keypair_t   *keypair;       /* Paire de clés pour signer les messages */
    char               own_alias[33]; /* Alias de ce device */
} espnow_config_t;

/**
 * Point d'entrée de la tâche FreeRTOS ESP-NOW.
 *
 * Boucle infinie qui :
 * 1. Attend des commandes sur cmd_queue (timeout 100ms)
 * 2. Traite les commandes (DISCOVER, SEND_TX, SEND_ACK)
 * 3. Le callback RX (appelé depuis le contexte ESP-NOW)
 *    décode les messages et poste des événements sur evt_queue
 *
 * @param param Pointeur vers espnow_config_t
 */
void espnow_task(void *param);

/* ================================================================
 * Fonctions internes exposées pour les tests
 * ================================================================ */

/**
 * Traiter un message reçu via ESP-NOW.
 *
 * Décode le type du message et génère l'événement approprié.
 * Appelé depuis le callback RX dans le contexte de la tâche.
 *
 * @param config  Configuration du module
 * @param src_mac Adresse MAC de l'émetteur
 * @param data    Données reçues
 * @param len     Taille des données
 */
void espnow_handle_rx(const espnow_config_t *config,
                       const uint8_t *src_mac,
                       const uint8_t *data, size_t len);

/**
 * Traiter une commande reçue depuis core/UI.
 *
 * @param config Configuration du module
 * @param cmd    Commande à traiter
 */
void espnow_handle_cmd(const espnow_config_t *config,
                        const comm_cmd_t *cmd);

#endif /* ESPNOW_H */
