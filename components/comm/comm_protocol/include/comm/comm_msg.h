/**
 * @file comm_msg.h
 * @brief Types de messages radio et fonctions de packing/unpacking.
 *
 * Tous les messages radio (ESP-NOW et LoRa) commencent par un octet
 * de type qui identifie le format du reste du paquet.
 *
 * Ce module fournit des helpers pour construire et décoder ces messages,
 * évitant la manipulation manuelle de buffers dans les couches supérieures.
 *
 * Contraintes de taille :
 * - ESP-NOW : 250 octets max
 * - LoRa : 255 octets max
 */

#ifndef COMM_MSG_H
#define COMM_MSG_H

#include "crypto/crypto_types.h"
#include "transaction/tx_types.h"
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* ================================================================
 * Types de messages radio
 * ================================================================ */

typedef enum {
    /* Messages ESP-NOW */
    COMM_MSG_DISCOVER   = 0x01, /* Broadcast : demande de découverte */
    COMM_MSG_ANNOUNCE   = 0x02, /* Unicast : réponse avec pubkey + alias */
    COMM_MSG_TX_LOCKED  = 0x03, /* Unicast : transaction verrouillée (CBOR) */
    COMM_MSG_TX_ACK     = 0x04, /* Unicast : accusé de réception */

    /* Messages LoRa */
    COMM_MSG_LORA_TX        = 0x10, /* TX confirmée unique */
    COMM_MSG_LORA_FRAG      = 0x11, /* Fragment de rattrapage */
    COMM_MSG_LORA_TIME_SYNC = 0x12, /* Sync temporelle maître → esclaves */
    COMM_MSG_LORA_BROADCAST = 0x13, /* Broadcast texte signé du maître */
    COMM_MSG_LORA_PING      = 0x14, /* Ping maître → tous les devices */
    COMM_MSG_LORA_PONG      = 0x15, /* Pong device → maître */
    COMM_MSG_LORA_SET_ALIAS       = 0x16, /* Renommage distant d'un device par le maître */
    COMM_MSG_LORA_SET_BENEFICIARY = 0x17, /* Configuration auto-forward bénéficiaire */
    COMM_MSG_LORA_ATTESTATION     = 0x18, /* [I2-fix] Attestation signée de confirmation d'une TX */
} comm_msg_type_t;

/* Tailles maximales des messages */
/*
 * [F-EN-001 / Lot E.1bis] ESP-NOW V2 (ESP-IDF 5.x) supporte jusqu'à
 * 1470 octets de payload, bien au-delà des 250 octets de V1. On
 * dimensionne la limite à 1 octet de type + 320 octets CBOR
 * (TX_CBOR_MAX_SIZE) pour permettre d'envoyer une transaction
 * complète en un seul paquet sans fragmentation.
 *
 * Avant ce fix, le buffer interne d'espnow_handle_cmd était plafonné
 * à 250 octets : toute TX sérialisée > 249 octets CBOR (deux parents
 * renseignés ou seq non-nul) provoquait l'échec silencieux de
 * comm_msg_pack_tx_locked() et l'abandon du paiement côté émetteur.
 *
 * La constante littérale (321) évite une dépendance circulaire vers
 * transaction/tx_serialize.h depuis comm_protocol. Si TX_CBOR_MAX_SIZE
 * évolue, mettre à jour ici également et vérifier la garde HAL dans
 * espnow_hal_esp32.c.
 */
#define COMM_MSG_ESPNOW_MAX   321  /* 1 octet type + TX_CBOR_MAX_SIZE (320) */

/*
 * [F-CP-006] COMM_MSG_LORA_MAX : limite physique d'un paquet LoRa
 * unique (modulation SX1262, charge utile par trame). C'est une
 * contrainte radio délibérément conservatrice : on s'aligne sur
 * 255 octets, valeur maximale acceptée par la majorité des SF/BW
 * supportés par le firmware (cf. components/device_hal/include/
 * lora_hal.h). Les TX dont la sérialisation CBOR dépasse cette
 * borne sont fragmentées par lora_sync (lora_frag_split,
 * LORA_FRAG_PAYLOAD_MAX = 251 octets utiles par fragment) puis
 * réassemblées côté récepteur. Les helpers comm_msg_pack_lora_tx
 * et comm_msg_pack_broadcast doivent rester sous cette limite.
 */
#define COMM_MSG_LORA_MAX     255  /* Limite radio LoRa par paquet (fragmentation au-delà) */
#define COMM_MSG_ALIAS_MAX     32  /* Alias device max */

/* ================================================================
 * Structures de messages décodés
 * ================================================================ */

/** Message DISCOVER : clé publique de l'émetteur */
typedef struct {
    public_key_t sender_key;
} comm_msg_discover_t;

/**
 * Message ANNOUNCE : réponse signée à un DISCOVER.
 *
 * Format wire : [0x02][pubkey:32][sig:64][nonce:4][alias_len:1][alias:N]
 * La signature couvre [nonce:4][alias_len:1][alias:N].
 */
typedef struct {
    public_key_t device_key;           /* Clé publique du device */
    signature_t  signature;            /* Signature Ed25519 de [nonce][alias_len][alias] */
    uint32_t     nonce;                /* Nonce anti-rejeu */
    char         alias[COMM_MSG_ALIAS_MAX + 1]; /* Alias null-terminated */
    uint8_t      alias_len;            /* Longueur de l'alias */
} comm_msg_announce_t;

/**
 * Message TX_ACK : accusé de réception signé d'une transaction.
 *
 * Format wire : [0x04][sender_pubkey:32][sig:64][nonce:4][tx_id:32]
 * La signature couvre [nonce:4][tx_id:32].
 */
typedef struct {
    public_key_t sender_key;           /* Clé publique de l'émetteur de l'ACK */
    signature_t  signature;            /* Signature Ed25519 de [nonce][tx_id] */
    uint32_t     nonce;                /* Nonce anti-rejeu */
    hash_t       tx_id;                /* Hash de la transaction confirmée */
} comm_msg_ack_t;

/**
 * Message LORA_TIME_SYNC : synchronisation temporelle maître.
 *
 * Envoyé par les devices maîtres toutes les 2 minutes via LoRa.
 * Permet aux devices normaux de calculer leur offset temporel.
 * Le message est signé par le maître (Ed25519) pour empêcher
 * l'injection de fausse synchronisation temporelle.
 *
 * Format wire : [0x12][pubkey:32][sig:64][timestamp:8 BE][lamport:8 BE]
 */
typedef struct {
    public_key_t master_key;       /* Clé publique du maître émetteur */
    signature_t  signature;        /* Signature Ed25519 de [timestamp:8][lamport:8] */
    uint64_t     master_timestamp; /* Temps wall-clock du maître (ms) */
    uint64_t     master_lamport;   /* Valeur Lamport du maître */
} comm_msg_time_sync_t;

/* Taille min du message ANNOUNCE signé : 1 (type) + 32 (pubkey) + 64 (sig) + 4 (nonce) + 1 (alias_len) = 102 */
#define COMM_MSG_ANNOUNCE_MIN_SIZE 102

/* Taille fixe du message TX_ACK signé : 1 (type) + 32 (pubkey) + 64 (sig) + 4 (nonce) + 32 (tx_id) = 133 */
#define COMM_MSG_ACK_SIZE 133

/**
 * Taille max du texte dans un broadcast maître.
 *
 * Calcul : 255 (max LoRa) - 1 (type) - 32 (pubkey) - 64 (sig) - 1 (text_len) = 157
 */
#define COMM_MSG_BROADCAST_TEXT_MAX 157

/* Taille min du message BROADCAST : type + pubkey + sig + text_len (sans texte) = 98 */
#define COMM_MSG_BROADCAST_MIN_SIZE 98

/**
 * Message LORA_BROADCAST : message texte signé du maître.
 *
 * Envoyé par un device maître via LoRa. Le texte est signé avec la clé
 * privée du maître (Ed25519). Les récepteurs vérifient que la pubkey
 * appartient à un maître connu (mint_authorities) et que la signature
 * est valide. Si la signature est invalide, le message est ignoré.
 *
 * Format wire : [0x13][pubkey:32][sig:64][text_len:1][text:N]
 */
typedef struct {
    public_key_t sender_key;                           /* Clé publique du maître émetteur */
    signature_t  signature;                            /* Signature Ed25519 de [text_len][text] */
    uint8_t      text_len;                             /* Longueur du texte (1..157) */
    char         text[COMM_MSG_BROADCAST_TEXT_MAX + 1]; /* Texte null-terminated */
} comm_msg_broadcast_t;

/* Taille du message TIME_SYNC : 1 (type) + 32 (pubkey) + 64 (sig) + 8 (timestamp) + 8 (lamport) = 113 */
#define COMM_MSG_TIME_SYNC_SIZE 113

/**
 * Message LORA_PING : demande de découverte LoRa signée par le maître.
 *
 * Envoyé par un device maître pour identifier les devices à portée.
 * Les récepteurs répondent avec un PONG et relayent le PING (single-hop).
 * Le message est signé par le maître (Ed25519) pour empêcher
 * l'usurpation d'identité lors de la découverte réseau.
 *
 * Format wire : [0x14][master_pubkey:32][sig:64][ping_id:2 BE]
 * La signature couvre [ping_id:2 BE]
 */
typedef struct {
    public_key_t master_key; /* Clé publique du maître émetteur */
    signature_t  signature;  /* Signature Ed25519 de [ping_id:2 BE] */
    uint16_t     ping_id;    /* Identifiant de session (corrélation PONG) */
} comm_msg_ping_t;

/* Taille du message PING : 1 (type) + 32 (pubkey) + 64 (sig) + 2 (ping_id) = 99 */
#define COMM_MSG_PING_SIZE 99

/**
 * Message LORA_PONG : réponse à un PING.
 *
 * Envoyé par chaque device qui reçoit un PING (directement ou via relay).
 * Contient l'identité du device répondant (pubkey + alias) et une
 * signature Ed25519 pour empêcher l'usurpation d'identité.
 * Les PONGs ne sont PAS relayés.
 *
 * Format wire : [0x15][device_pubkey:32][sig:64][ping_id:2 BE][alias_len:1][alias:N]
 * La signature couvre [ping_id:2 BE][alias_len:1][alias:N].
 *
 * [I4-fix] Avant, les PONG étaient non signés : un attaquant pouvait
 * polluer la liste de scan/rename/forward avec de fausses identités
 * en répondant à un PING avec une pubkey qu'il ne possède pas.
 */
typedef struct {
    public_key_t device_key;                       /* Clé publique du device répondant */
    signature_t  signature;                        /* Signature Ed25519 de [ping_id:2 BE][alias_len:1][alias:N] */
    uint16_t     ping_id;                          /* Identifiant de session (copié du PING) */
    char         alias[COMM_MSG_ALIAS_MAX + 1];    /* Alias null-terminated */
    uint8_t      alias_len;                        /* Longueur de l'alias */
} comm_msg_pong_t;

/* Taille min PONG : 1 (type) + 32 (pubkey) + 64 (sig) + 2 (ping_id) + 1 (alias_len) = 100 */
#define COMM_MSG_PONG_MIN_SIZE 100

/**
 * Message LORA_ATTESTATION : [I2-fix] attestation signée de confirmation.
 *
 * Diffusée par le destinataire d'une TX TRANSFER après qu'il ait reçu et
 * verifié la transaction. Permet au reste du réseau (hors portée ESP-NOW)
 * de passer cette TX de LOCKED à CONFIRMED de manière prouvée.
 *
 * Avant ce message, les TX reçues par LoRa étaient forcées en LOCKED côté
 * récepteur (durcissement anti-usurpation du status), ce qui empêchait
 * la convergence du ledger : un tiers-relai ne pouvait jamais marquer
 * une TX comme finale. L'attestation signée comble ce trou en fournissant
 * une preuve cryptographique de confirmation.
 *
 * Format wire : [0x18][attester_pubkey:32][sig:64][tx_id:32]
 * La signature couvre [tx_id:32].
 *
 * Le récepteur vérifie :
 *  1. Signature Ed25519 valide contre attester_pubkey
 *  2. tx = dag_get_by_id(tx_id) existe
 *  3. attester_pubkey == tx.to (seul le destinataire peut attester)
 *  4. Si tout OK → dag_set_status(tx_id, CONFIRMED)
 */
typedef struct {
    public_key_t attester_key;  /* Clé publique du destinataire attestant */
    signature_t  signature;     /* Signature Ed25519 de tx_id */
    hash_t       tx_id;         /* Hash de la TX attestée */
} comm_msg_attestation_t;

/* Taille fixe ATTESTATION : 1 (type) + 32 (pubkey) + 64 (sig) + 32 (tx_id) = 129 */
#define COMM_MSG_ATTESTATION_SIZE 129

/**
 * Message LORA_SET_ALIAS : renommage distant d'un device par le maître.
 *
 * Permet au maître de changer l'alias d'un device client à distance.
 * Le message est signé par le maître (Ed25519) pour authentifier l'émetteur.
 * Le device cible vérifie que target_key correspond à sa propre clé publique.
 *
 * Format wire : [0x16][master_pubkey:32][sig:64][target_pubkey:32][alias_len:1][alias:N]
 * La signature couvre [target_pubkey:32][alias_len:1][alias:N]
 */
typedef struct {
    public_key_t master_key;                        /* Clé publique du maître émetteur */
    signature_t  signature;                         /* Signature Ed25519 de [target_key][alias_len][alias] */
    public_key_t target_key;                        /* Clé publique du device cible */
    uint8_t      alias_len;                         /* Longueur du nouvel alias (1..32) */
    char         alias[COMM_MSG_ALIAS_MAX + 1];     /* Nouvel alias null-terminated */
} comm_msg_set_alias_t;

/* Taille min du message SET_ALIAS : 1 (type) + 32 (master_key) + 64 (sig) + 32 (target_key) + 1 (alias_len) = 130 */
#define COMM_MSG_SET_ALIAS_MIN_SIZE 130

/**
 * Message LORA_SET_BENEFICIARY : configuration auto-forward par le maître.
 *
 * Permet au maître de configurer un device pour qu'il transfère
 * périodiquement son solde vers une clé publique bénéficiaire.
 * Si beneficiary_key est all-zeros, le mode est désactivé.
 *
 * Format wire : [0x17][master_key:32][sig:64][target_key:32][beneficiary_key:32][interval:2 BE]
 * La signature couvre [target_key:32][beneficiary_key:32][interval:2 BE]
 */
typedef struct {
    public_key_t master_key;            /* Clé publique du maître émetteur */
    signature_t  signature;             /* Signature Ed25519 de [target][beneficiary][interval] */
    public_key_t target_key;            /* Clé publique du device cible */
    public_key_t beneficiary_key;       /* Destinataire des forwards (all-zeros = désactiver) */
    uint16_t     forward_interval_min;  /* Intervalle de forward en minutes (min 1) */
} comm_msg_set_beneficiary_t;

/* Taille fixe du message SET_BENEFICIARY : 1 + 32 + 64 + 32 + 32 + 2 = 163 */
#define COMM_MSG_SET_BENEFICIARY_SIZE 163

/* ================================================================
 * Fonctions de packing (struct → buffer radio)
 * ================================================================ */

/**
 * Construire un message DISCOVER dans un buffer.
 *
 * Format : [0x01][pubkey:32]
 *
 * @param buf     Buffer de sortie (min 33 octets)
 * @param buf_len Taille du buffer
 * @param key     Clé publique de l'émetteur
 * @param out_len [out] Nombre d'octets écrits
 * @return 0 en cas de succès, -1 si buffer trop petit ou paramètre NULL
 */
int comm_msg_pack_discover(uint8_t *buf, size_t buf_len,
                           const public_key_t *key, size_t *out_len);

/**
 * Construire un message ANNOUNCE signé dans un buffer.
 *
 * Format : [0x02][pubkey:32][sig:64][nonce:4][alias_len:1][alias:N]
 * La signature doit avoir été calculée au préalable sur [nonce:4][alias_len:1][alias:N].
 *
 * @param buf       Buffer de sortie (min COMM_MSG_ANNOUNCE_MIN_SIZE + alias_len)
 * @param buf_len   Taille du buffer
 * @param key       Clé publique du device
 * @param sig       Signature Ed25519 de [nonce][alias_len][alias]
 * @param nonce     Nonce anti-rejeu
 * @param alias     Alias du device (null-terminated)
 * @param alias_len Longueur de l'alias (max COMM_MSG_ALIAS_MAX)
 * @param out_len   [out] Nombre d'octets écrits
 * @return 0 en cas de succès, -1 si erreur
 */
int comm_msg_pack_announce(uint8_t *buf, size_t buf_len,
                           const public_key_t *key,
                           const signature_t *sig,
                           uint32_t nonce,
                           const char *alias, uint8_t alias_len,
                           size_t *out_len);

/**
 * Construire un message TX_LOCKED dans un buffer.
 *
 * Format : [0x03][CBOR TX sérialisée]
 * Utilise tx_serialize_full() en interne.
 *
 * @param buf     Buffer de sortie (min 250 octets recommandé)
 * @param buf_len Taille du buffer
 * @param tx      Transaction à envoyer
 * @param out_len [out] Nombre d'octets écrits
 * @return 0 en cas de succès, -1 si erreur de sérialisation
 */
int comm_msg_pack_tx_locked(uint8_t *buf, size_t buf_len,
                            const transaction_t *tx, size_t *out_len);

/**
 * Construire un message TX_ACK signé dans un buffer.
 *
 * Format : [0x04][sender_pubkey:32][sig:64][nonce:4][tx_id:32]
 * La signature doit avoir été calculée au préalable sur [nonce:4][tx_id:32].
 *
 * @param buf        Buffer de sortie (min COMM_MSG_ACK_SIZE octets)
 * @param buf_len    Taille du buffer
 * @param sender_key Clé publique de l'émetteur de l'ACK
 * @param sig        Signature Ed25519 de [nonce][tx_id]
 * @param nonce      Nonce anti-rejeu
 * @param tx_id      Hash de la transaction confirmée
 * @param out_len    [out] Nombre d'octets écrits
 * @return 0 en cas de succès, -1 si erreur
 */
int comm_msg_pack_ack(uint8_t *buf, size_t buf_len,
                      const public_key_t *sender_key,
                      const signature_t *sig,
                      uint32_t nonce,
                      const hash_t *tx_id,
                      size_t *out_len);

/**
 * Construire un message LORA_TX dans un buffer.
 *
 * Format : [0x10][CBOR TX sérialisée]
 *
 * @param buf     Buffer de sortie (min 251 octets recommandé)
 * @param buf_len Taille du buffer
 * @param tx      Transaction confirmée à broadcaster
 * @param out_len [out] Nombre d'octets écrits
 * @return 0 en cas de succès, -1 si erreur de sérialisation
 */
int comm_msg_pack_lora_tx(uint8_t *buf, size_t buf_len,
                          const transaction_t *tx, size_t *out_len);

/* ================================================================
 * Fonctions d'unpacking (buffer radio → struct)
 * ================================================================ */

/**
 * Identifier le type d'un message à partir de son premier octet.
 *
 * @param buf     Buffer du message reçu
 * @param buf_len Taille du buffer
 * @param type    [out] Type du message
 * @return 0 en cas de succès, -1 si buffer vide ou type inconnu
 */
int comm_msg_get_type(const uint8_t *buf, size_t buf_len,
                      comm_msg_type_t *type);

/**
 * Décoder un message DISCOVER.
 *
 * @param buf     Buffer du message (doit commencer par 0x01)
 * @param buf_len Taille du buffer
 * @param msg     [out] Message décodé
 * @return 0 en cas de succès, -1 si format invalide
 */
int comm_msg_unpack_discover(const uint8_t *buf, size_t buf_len,
                             comm_msg_discover_t *msg);

/**
 * Décoder un message ANNOUNCE.
 *
 * @param buf     Buffer du message (doit commencer par 0x02)
 * @param buf_len Taille du buffer
 * @param msg     [out] Message décodé
 * @return 0 en cas de succès, -1 si format invalide
 */
int comm_msg_unpack_announce(const uint8_t *buf, size_t buf_len,
                             comm_msg_announce_t *msg);

/**
 * Décoder un message TX_LOCKED — extrait la transaction CBOR.
 *
 * @param buf     Buffer du message (doit commencer par 0x03)
 * @param buf_len Taille du buffer
 * @param tx      [out] Transaction désérialisée
 * @return 0 en cas de succès, -1 si CBOR invalide
 */
int comm_msg_unpack_tx_locked(const uint8_t *buf, size_t buf_len,
                              transaction_t *tx);

/**
 * Décoder un message TX_ACK.
 *
 * @param buf     Buffer du message (doit commencer par 0x04)
 * @param buf_len Taille du buffer
 * @param msg     [out] ACK décodé
 * @return 0 en cas de succès, -1 si format invalide
 */
int comm_msg_unpack_ack(const uint8_t *buf, size_t buf_len,
                        comm_msg_ack_t *msg);

/**
 * Décoder un message LORA_TX — extrait la transaction CBOR.
 *
 * @param buf     Buffer du message (doit commencer par 0x10)
 * @param buf_len Taille du buffer
 * @param tx      [out] Transaction désérialisée
 * @return 0 en cas de succès, -1 si CBOR invalide
 */
int comm_msg_unpack_lora_tx(const uint8_t *buf, size_t buf_len,
                            transaction_t *tx);

/**
 * Construire un message LORA_TIME_SYNC signé dans un buffer.
 *
 * Format : [0x12][pubkey:32][sig:64][timestamp:8 BE][lamport:8 BE]
 * La signature doit avoir été calculée au préalable sur [timestamp:8 BE][lamport:8 BE].
 *
 * @param buf              Buffer de sortie (min COMM_MSG_TIME_SYNC_SIZE octets)
 * @param buf_len          Taille du buffer
 * @param master_key       Clé publique du maître
 * @param sig              Signature Ed25519 de [timestamp:8][lamport:8]
 * @param master_timestamp Temps wall-clock du maître (ms)
 * @param master_lamport   Valeur Lamport du maître
 * @param out_len          [out] Nombre d'octets écrits
 * @return 0 en cas de succès, -1 si erreur
 */
int comm_msg_pack_time_sync(uint8_t *buf, size_t buf_len,
                            const public_key_t *master_key,
                            const signature_t *sig,
                            uint64_t master_timestamp,
                            uint64_t master_lamport,
                            size_t *out_len);

/**
 * Construire un message LORA_BROADCAST dans un buffer.
 *
 * Format : [0x13][pubkey:32][sig:64][text_len:1][text:N]
 * La signature doit avoir été calculée au préalable sur [text_len:1][text:N].
 *
 * @param buf        Buffer de sortie (min COMM_MSG_BROADCAST_MIN_SIZE + text_len)
 * @param buf_len    Taille du buffer
 * @param sender_key Clé publique du maître émetteur
 * @param sig        Signature Ed25519 de [text_len][text]
 * @param text       Texte du message (null-terminated)
 * @param text_len   Longueur du texte (1..COMM_MSG_BROADCAST_TEXT_MAX)
 * @param out_len    [out] Nombre d'octets écrits
 * @return 0 en cas de succès, -1 si erreur
 */
int comm_msg_pack_broadcast(uint8_t *buf, size_t buf_len,
                            const public_key_t *sender_key,
                            const signature_t *sig,
                            const char *text, uint8_t text_len,
                            size_t *out_len);

/**
 * Décoder un message LORA_BROADCAST.
 *
 * @param buf     Buffer du message (doit commencer par 0x13)
 * @param buf_len Taille du buffer
 * @param msg     [out] Message décodé
 * @return 0 en cas de succès, -1 si format invalide
 */
int comm_msg_unpack_broadcast(const uint8_t *buf, size_t buf_len,
                              comm_msg_broadcast_t *msg);

/**
 * Décoder un message LORA_TIME_SYNC.
 *
 * @param buf     Buffer du message (doit commencer par 0x12)
 * @param buf_len Taille du buffer
 * @param msg     [out] Message décodé
 * @return 0 en cas de succès, -1 si format invalide
 */
int comm_msg_unpack_time_sync(const uint8_t *buf, size_t buf_len,
                              comm_msg_time_sync_t *msg);

/**
 * Construire un message LORA_PING signé dans un buffer.
 *
 * Format : [0x14][master_pubkey:32][sig:64][ping_id:2 BE]
 * La signature doit avoir été calculée au préalable sur [ping_id:2 BE].
 *
 * @param buf        Buffer de sortie (min COMM_MSG_PING_SIZE octets)
 * @param buf_len    Taille du buffer
 * @param master_key Clé publique du maître
 * @param sig        Signature Ed25519 de [ping_id:2 BE]
 * @param ping_id    Identifiant de session
 * @param out_len    [out] Nombre d'octets écrits
 * @return 0 en cas de succès, -1 si erreur
 */
int comm_msg_pack_ping(uint8_t *buf, size_t buf_len,
                       const public_key_t *master_key,
                       const signature_t *sig,
                       uint16_t ping_id, size_t *out_len);

/**
 * Décoder un message LORA_PING.
 *
 * @param buf     Buffer du message (doit commencer par 0x14)
 * @param buf_len Taille du buffer
 * @param msg     [out] Message décodé
 * @return 0 en cas de succès, -1 si format invalide
 */
int comm_msg_unpack_ping(const uint8_t *buf, size_t buf_len,
                         comm_msg_ping_t *msg);

/**
 * Construire un message LORA_PONG signé dans un buffer.
 *
 * Format : [0x15][device_pubkey:32][sig:64][ping_id:2 BE][alias_len:1][alias:N]
 * Le signataire signe [ping_id:2 BE][alias_len:1][alias:N].
 *
 * @param buf        Buffer de sortie (min COMM_MSG_PONG_MIN_SIZE + alias_len)
 * @param buf_len    Taille du buffer
 * @param device_key Clé publique du device répondant
 * @param sig        Signature Ed25519 de [ping_id:2 BE][alias_len:1][alias:N]
 * @param ping_id    Identifiant de session (copié du PING)
 * @param alias      Alias du device (null-terminated)
 * @param alias_len  Longueur de l'alias (max COMM_MSG_ALIAS_MAX)
 * @param out_len    [out] Nombre d'octets écrits
 * @return 0 en cas de succès, -1 si erreur
 */
int comm_msg_pack_pong(uint8_t *buf, size_t buf_len,
                       const public_key_t *device_key,
                       const signature_t *sig,
                       uint16_t ping_id,
                       const char *alias, uint8_t alias_len,
                       size_t *out_len);

/**
 * Décoder un message LORA_PONG.
 *
 * @param buf     Buffer du message (doit commencer par 0x15)
 * @param buf_len Taille du buffer
 * @param msg     [out] Message décodé
 * @return 0 en cas de succès, -1 si format invalide
 */
int comm_msg_unpack_pong(const uint8_t *buf, size_t buf_len,
                         comm_msg_pong_t *msg);

/**
 * [I2-fix] Construire un message LORA_ATTESTATION signé dans un buffer.
 *
 * Format : [0x18][attester_pubkey:32][sig:64][tx_id:32]
 * La signature doit avoir été calculée au préalable sur [tx_id:32].
 *
 * @param buf           Buffer de sortie (min COMM_MSG_ATTESTATION_SIZE)
 * @param buf_len       Taille du buffer
 * @param attester_key  Clé publique du destinataire attestant
 * @param sig           Signature Ed25519 de tx_id
 * @param tx_id         Hash de la TX attestée
 * @param out_len       [out] Nombre d'octets écrits (= 129)
 * @return 0 en cas de succès, -1 si erreur
 */
int comm_msg_pack_attestation(uint8_t *buf, size_t buf_len,
                              const public_key_t *attester_key,
                              const signature_t *sig,
                              const hash_t *tx_id,
                              size_t *out_len);

/**
 * [I2-fix] Décoder un message LORA_ATTESTATION.
 *
 * @param buf     Buffer du message (doit commencer par 0x18, taille 129 octets)
 * @param buf_len Taille du buffer
 * @param msg     [out] Message décodé
 * @return 0 en cas de succès, -1 si format invalide
 */
int comm_msg_unpack_attestation(const uint8_t *buf, size_t buf_len,
                                comm_msg_attestation_t *msg);

/**
 * Construire un message LORA_SET_ALIAS dans un buffer.
 *
 * Format : [0x16][master_pubkey:32][sig:64][target_pubkey:32][alias_len:1][alias:N]
 * La signature doit avoir été calculée au préalable sur [target_key:32][alias_len:1][alias:N].
 *
 * @param buf        Buffer de sortie (min COMM_MSG_SET_ALIAS_MIN_SIZE + alias_len)
 * @param buf_len    Taille du buffer
 * @param master_key Clé publique du maître émetteur
 * @param sig        Signature Ed25519 de [target_key][alias_len][alias]
 * @param target_key Clé publique du device cible
 * @param alias      Nouvel alias (null-terminated)
 * @param alias_len  Longueur du nouvel alias (1..COMM_MSG_ALIAS_MAX)
 * @param out_len    [out] Nombre d'octets écrits
 * @return 0 en cas de succès, -1 si erreur
 */
int comm_msg_pack_set_alias(uint8_t *buf, size_t buf_len,
                            const public_key_t *master_key,
                            const signature_t *sig,
                            const public_key_t *target_key,
                            const char *alias, uint8_t alias_len,
                            size_t *out_len);

/**
 * Décoder un message LORA_SET_ALIAS.
 *
 * @param buf     Buffer du message (doit commencer par 0x16)
 * @param buf_len Taille du buffer
 * @param msg     [out] Message décodé
 * @return 0 en cas de succès, -1 si format invalide
 */
int comm_msg_unpack_set_alias(const uint8_t *buf, size_t buf_len,
                              comm_msg_set_alias_t *msg);

/**
 * Construire un message LORA_SET_BENEFICIARY dans un buffer.
 *
 * Format : [0x17][master_key:32][sig:64][target_key:32][beneficiary_key:32][interval:2 BE]
 * La signature doit avoir été calculée sur [target_key][beneficiary_key][interval:2 BE].
 *
 * @param buf              Buffer de sortie (min COMM_MSG_SET_BENEFICIARY_SIZE octets)
 * @param buf_len          Taille du buffer
 * @param master_key       Clé publique du maître émetteur
 * @param sig              Signature Ed25519
 * @param target_key       Clé publique du device cible
 * @param beneficiary_key  Clé publique du bénéficiaire (all-zeros = désactiver)
 * @param forward_interval_min Intervalle en minutes
 * @param out_len          [out] Nombre d'octets écrits
 * @return 0 en cas de succès, -1 si erreur
 */
int comm_msg_pack_set_beneficiary(uint8_t *buf, size_t buf_len,
                                  const public_key_t *master_key,
                                  const signature_t *sig,
                                  const public_key_t *target_key,
                                  const public_key_t *beneficiary_key,
                                  uint16_t forward_interval_min,
                                  size_t *out_len);

/**
 * Décoder un message LORA_SET_BENEFICIARY.
 *
 * @param buf     Buffer du message (doit commencer par 0x17)
 * @param buf_len Taille du buffer
 * @param msg     [out] Message décodé
 * @return 0 en cas de succès, -1 si format invalide
 */
int comm_msg_unpack_set_beneficiary(const uint8_t *buf, size_t buf_len,
                                    comm_msg_set_beneficiary_t *msg);

/* ================================================================
 * Verification de signatures [Lot B item 4]
 *
 * Reconstituent le buffer canonical signe pour chaque type de message
 * LoRa et appellent crypto_verify(). Permettent a la couche LoRa de
 * filtrer les messages mal signes avant de les poster dans la queue
 * d'evenements core_task. core_task continue de faire ses propres
 * verifications d'autorite/identite (defense en profondeur).
 *
 * Convention : 0 si signature valide, -1 sinon (identique pack/unpack).
 * ================================================================ */

/**
 * Verifier la signature Ed25519 d'un message LORA_BROADCAST decode.
 * Couvre [text_len:1][text:N], signe par msg->sender_key.
 *
 * @param msg Message decode (sender_key, signature, text, text_len)
 * @return 0 si signature valide, -1 sinon
 */
int comm_msg_verify_broadcast(const comm_msg_broadcast_t *msg);

/**
 * Verifier la signature Ed25519 d'un message LORA_SET_ALIAS decode.
 * Couvre [target_key:32][alias_len:1][alias:N], signe par msg->master_key.
 *
 * @param msg Message decode
 * @return 0 si signature valide, -1 sinon
 */
int comm_msg_verify_set_alias(const comm_msg_set_alias_t *msg);

/**
 * Verifier la signature Ed25519 d'un message LORA_SET_BENEFICIARY decode.
 * Couvre [target_key:32][beneficiary_key:32][interval:2 BE],
 * signe par msg->master_key.
 *
 * @param msg Message decode
 * @return 0 si signature valide, -1 sinon
 */
int comm_msg_verify_set_beneficiary(const comm_msg_set_beneficiary_t *msg);

/**
 * [F-CP-003] Verifier la signature Ed25519 d'un message LORA_ATTESTATION
 * decode. Couvre [tx_id:32], signe par msg->attester_key.
 *
 * Symmetrique aux trois autres comm_msg_verify_* : permet a la couche
 * LoRa de filtrer les attestations forgees AVANT de poster un evenement
 * vers core_task (defense en profondeur). La validation finale "attester
 * == tx.to" reste a la charge de core_task qui a acces au DAG.
 *
 * @param msg Message decode (attester_key, signature, tx_id)
 * @return 0 si signature valide, -1 sinon
 */
int comm_msg_verify_attestation(const comm_msg_attestation_t *msg);

#endif /* COMM_MSG_H */
