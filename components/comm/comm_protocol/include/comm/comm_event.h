/**
 * @file comm_event.h
 * @brief Événements et commandes pour la communication inter-tâches.
 *
 * Deux queues FreeRTOS connectent la couche comm au reste du système :
 *
 * 1. comm_evt_queue (comm → core_task) :
 *    Événements générés par espnow et lora_sync quand un message radio
 *    est reçu. Le core_task consomme ces événements et met à jour le
 *    DAG, le wallet, et les verrous.
 *
 * 2. espnow_cmd_queue (core/UI → espnow_task) :
 *    Commandes envoyées par le core_task ou l'UI pour déclencher des
 *    actions radio (lancer un DISCOVER, envoyer une TX, envoyer un ACK).
 *
 * Aucune de ces structures ne contient de pointeur vers de la mémoire
 * dynamique — tout est copié par valeur dans la queue.
 */

#ifndef COMM_EVENT_H
#define COMM_EVENT_H

#include "crypto/crypto_types.h"
#include "transaction/tx_types.h"
#include "comm/comm_msg.h"
#include <stdint.h>
#include <stdbool.h>

/* ================================================================
 * Événements comm → core_task
 * ================================================================ */

/** Types d'événements générés par la couche comm */
typedef enum {
    COMM_EVT_PEER_DISCOVERED,   /* Un peer a répondu au DISCOVER */
    COMM_EVT_TX_RECEIVED,       /* TX LOCKED reçue via ESP-NOW */
    COMM_EVT_ACK_RECEIVED,      /* ACK reçu (paiement accepté) */
    COMM_EVT_TX_TIMEOUT,        /* Timeout 30s sans ACK */
    COMM_EVT_LORA_TX_RECEIVED,    /* TX confirmée reçue via LoRa */
    COMM_EVT_TIME_SYNC_RECEIVED,  /* Sync temporelle maître reçue via LoRa */
    COMM_EVT_BROADCAST_RECEIVED,  /* Message texte signé du maître via LoRa */
    COMM_EVT_PING_RECEIVED,       /* Ping maître reçu via LoRa */
    COMM_EVT_PONG_RECEIVED,       /* Pong device reçu via LoRa */
    COMM_EVT_SET_ALIAS_RECEIVED,        /* Renommage distant reçu via LoRa */
    COMM_EVT_SET_BENEFICIARY_RECEIVED,  /* Configuration bénéficiaire reçue via LoRa */
    COMM_EVT_ATTESTATION_RECEIVED,      /* [I2-fix] Attestation de confirmation signée via LoRa */
} comm_event_type_t;

/**
 * Informations sur un peer découvert via ESP-NOW.
 *
 * Contient tout ce qu'il faut pour afficher le peer dans l'UI
 * et lui envoyer un message unicast (adresse MAC).
 */
typedef struct {
    public_key_t public_key;     /* Clé publique Ed25519 du peer */
    uint8_t      mac_addr[6];    /* Adresse MAC pour envoi ESP-NOW */
    char         alias[33];      /* Alias du device (null-terminated) */
} comm_peer_info_t;

/**
 * Événement généré par la couche comm.
 *
 * Taille ~240 octets (dominée par transaction_t dans l'union).
 * Passé par valeur dans la queue FreeRTOS.
 */
typedef struct {
    comm_event_type_t type;

    /** Adresse MAC source du message (pertinente pour TX_RECEIVED) [M7] */
    uint8_t src_mac[6];

    union {
        comm_peer_info_t     peer;      /* COMM_EVT_PEER_DISCOVERED */
        transaction_t        tx;        /* COMM_EVT_TX_RECEIVED, COMM_EVT_LORA_TX_RECEIVED */
        hash_t               tx_id;     /* COMM_EVT_TX_TIMEOUT */
        /* [C4-fix] L'ACK transporte le tx_id ET la cle publique du
         * signataire (destinataire suppose). Le handler verifie que
         * sender_key correspond bien au `to` de la transaction verrouillee
         * pour empecher un tiers de confirmer a la place du vrai
         * destinataire (attaque par forge d'ACK). */
        struct {
            hash_t       tx_id;
            public_key_t sender_key;
        } ack;                          /* COMM_EVT_ACK_RECEIVED */
        comm_msg_time_sync_t time_sync;  /* COMM_EVT_TIME_SYNC_RECEIVED */
        comm_msg_broadcast_t broadcast;  /* COMM_EVT_BROADCAST_RECEIVED */
        comm_msg_ping_t      ping;       /* COMM_EVT_PING_RECEIVED */
        comm_msg_pong_t      pong;       /* COMM_EVT_PONG_RECEIVED */
        comm_msg_set_alias_t       set_alias;       /* COMM_EVT_SET_ALIAS_RECEIVED */
        comm_msg_set_beneficiary_t set_beneficiary;  /* COMM_EVT_SET_BENEFICIARY_RECEIVED */
        comm_msg_attestation_t     attestation;      /* COMM_EVT_ATTESTATION_RECEIVED */
    } data;
} comm_event_t;

/* ================================================================
 * Commandes core/UI → espnow_task
 * ================================================================ */

/** Types de commandes envoyées vers espnow_task */
typedef enum {
    COMM_CMD_START_DISCOVER, /* Lancer un broadcast DISCOVER */
    COMM_CMD_SEND_TX,        /* Envoyer une TX LOCKED à un peer */
    COMM_CMD_SEND_ACK,       /* Envoyer un ACK à un peer */
} comm_cmd_type_t;

/**
 * Commande envoyée par core/UI vers espnow_task.
 *
 * Pour START_DISCOVER, aucune donnée supplémentaire n'est nécessaire.
 * Pour SEND_TX et SEND_ACK, l'adresse MAC de destination est requise.
 */
typedef struct {
    comm_cmd_type_t type;

    union {
        /* COMM_CMD_SEND_TX : transaction + MAC destination */
        struct {
            transaction_t tx;
            uint8_t       dest_mac[6];
        } send_tx;

        /* COMM_CMD_SEND_ACK : hash TX + MAC destination */
        struct {
            hash_t  tx_id;
            uint8_t dest_mac[6];
        } send_ack;
    } data;
} comm_cmd_t;

#endif /* COMM_EVENT_H */
