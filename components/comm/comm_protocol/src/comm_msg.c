/**
 * @file comm_msg.c
 * @brief Packing/unpacking des messages radio.
 *
 * Chaque message commence par un octet de type suivi du payload.
 * Les fonctions pack_* construisent le buffer à envoyer.
 * Les fonctions unpack_* extraient les données d'un buffer reçu.
 *
 * Pour les messages contenant des transactions CBOR (TX_LOCKED, LORA_TX),
 * on délègue à tx_serialize_full() / tx_deserialize() du module transaction.
 */

#include "comm/comm_msg.h"
#include "transaction/tx_serialize.h"
#include "crypto/crypto_sign.h"
#include "esp_err.h"
#include <string.h>

/* ================================================================
 * Forward declarations des helpers big-endian
 * ================================================================ */

static void     write_u16_be(uint8_t *dst, uint16_t val);
static uint16_t read_u16_be(const uint8_t *src);
static void     write_u32_be(uint8_t *dst, uint32_t val);
static uint32_t read_u32_be(const uint8_t *src);
static void     write_u64_be(uint8_t *dst, uint64_t val);
static uint64_t read_u64_be(const uint8_t *src);

/* ================================================================
 * Packing
 * ================================================================ */

int comm_msg_pack_discover(uint8_t *buf, size_t buf_len,
                           const public_key_t *key, size_t *out_len)
{
    /* Taille nécessaire : 1 (type) + 32 (pubkey) = 33 octets */
    if (!buf || !key || !out_len || buf_len < 33) {
        return -1;
    }

    buf[0] = COMM_MSG_DISCOVER;
    memcpy(&buf[1], key->bytes, CRYPTO_PUBLIC_KEY_SIZE);
    *out_len = 33;
    return 0;
}

int comm_msg_pack_announce(uint8_t *buf, size_t buf_len,
                           const public_key_t *key,
                           const signature_t *sig,
                           uint32_t nonce,
                           const char *alias, uint8_t alias_len,
                           size_t *out_len)
{
    if (!buf || !key || !sig || !alias || !out_len) {
        return -1;
    }

    /*
     * [F-CP-004] alias_len == 0 est LÉGAL ici, contrairement à
     * pack_set_alias et pack_broadcast qui rejettent une longueur
     * nulle. Un ANNOUNCE peut être émis par un device en cours
     * d'initialisation qui n'a pas encore reçu d'alias du maître :
     * il s'annonce avec sa clé publique seule. Le contrat de l'API
     * est : alias_len == 0 produit un paquet ANNOUNCE valide sans
     * texte d'alias (102 octets exactement). Les couches supérieures
     * qui exigent un alias non-vide doivent valider sémantiquement
     * en amont. Symétrique de unpack_announce qui accepte aussi 0.
     */

    /* Tronquer l'alias si trop long */
    if (alias_len > COMM_MSG_ALIAS_MAX) {
        alias_len = COMM_MSG_ALIAS_MAX;
    }

    /*
     * Format : [0x02][pubkey:32][sig:64][nonce:4][alias_len:1][alias:N]
     * Taille totale : 1 + 32 + 64 + 4 + 1 + N = 102 + N
     */
    size_t total = COMM_MSG_ANNOUNCE_MIN_SIZE + alias_len;
    if (buf_len < total) {
        return -1;
    }

    size_t offset = 0;

    buf[offset++] = COMM_MSG_ANNOUNCE;                                   /* type : 1 octet */
    memcpy(&buf[offset], key->bytes, CRYPTO_PUBLIC_KEY_SIZE);            /* pubkey : 32 octets */
    offset += CRYPTO_PUBLIC_KEY_SIZE;
    memcpy(&buf[offset], sig->bytes, CRYPTO_SIGNATURE_SIZE);             /* sig : 64 octets */
    offset += CRYPTO_SIGNATURE_SIZE;
    write_u32_be(&buf[offset], nonce);                                   /* nonce : 4 octets BE */
    offset += 4;
    buf[offset++] = alias_len;                                           /* alias_len : 1 octet */
    memcpy(&buf[offset], alias, alias_len);                              /* alias : N octets */
    offset += alias_len;

    *out_len = offset;
    return 0;
}

int comm_msg_pack_tx_locked(uint8_t *buf, size_t buf_len,
                            const transaction_t *tx, size_t *out_len)
{
    if (!buf || !tx || !out_len || buf_len < 2) {
        return -1;
    }

    /* Réserver le premier octet pour le type */
    buf[0] = COMM_MSG_TX_LOCKED;

    /* Sérialiser la TX en CBOR dans buf[1..] */
    size_t cbor_len = 0;
    esp_err_t err = tx_serialize_full(tx, &buf[1], buf_len - 1, &cbor_len);
    if (err != ESP_OK) {
        return -1;
    }

    *out_len = 1 + cbor_len;
    return 0;
}

int comm_msg_pack_ack(uint8_t *buf, size_t buf_len,
                      const public_key_t *sender_key,
                      const signature_t *sig,
                      uint32_t nonce,
                      const hash_t *tx_id,
                      size_t *out_len)
{
    if (!buf || !sender_key || !sig || !tx_id || !out_len) {
        return -1;
    }

    /*
     * Format : [0x04][sender_pubkey:32][sig:64][nonce:4][tx_id:32]
     * Taille totale : 133 octets (fixe)
     */
    if (buf_len < COMM_MSG_ACK_SIZE) {
        return -1;
    }

    size_t offset = 0;

    buf[offset++] = COMM_MSG_TX_ACK;                                    /* type : 1 octet */
    memcpy(&buf[offset], sender_key->bytes, CRYPTO_PUBLIC_KEY_SIZE);     /* sender_key : 32 octets */
    offset += CRYPTO_PUBLIC_KEY_SIZE;
    memcpy(&buf[offset], sig->bytes, CRYPTO_SIGNATURE_SIZE);             /* sig : 64 octets */
    offset += CRYPTO_SIGNATURE_SIZE;
    write_u32_be(&buf[offset], nonce);                                   /* nonce : 4 octets BE */
    offset += 4;
    memcpy(&buf[offset], tx_id->bytes, CRYPTO_HASH_SIZE);               /* tx_id : 32 octets */
    offset += CRYPTO_HASH_SIZE;

    *out_len = offset;
    return 0;
}

int comm_msg_pack_lora_tx(uint8_t *buf, size_t buf_len,
                          const transaction_t *tx, size_t *out_len)
{
    if (!buf || !tx || !out_len || buf_len < 2) {
        return -1;
    }

    /* Type LoRa TX */
    buf[0] = COMM_MSG_LORA_TX;

    /* Sérialiser la TX en CBOR dans buf[1..] */
    size_t cbor_len = 0;
    esp_err_t err = tx_serialize_full(tx, &buf[1], buf_len - 1, &cbor_len);
    if (err != ESP_OK) {
        return -1;
    }

    /*
     * [F-CP-001] Garde-fou borne haute : la sortie complète (1 octet
     * de type + CBOR) doit tenir dans un paquet LoRa physique
     * (COMM_MSG_LORA_MAX = 255 octets). tx_serialize_full ne connaît
     * que sa propre contrainte TX_CBOR_MAX_SIZE (320) qui correspond
     * à la limite ESP-NOW V2 ; sans cette vérification, une TX de
     * 256-320 octets serait acceptée par le pack puis tronquée ou
     * rejetée silencieusement par la radio LoRa. La fragmentation
     * (lora_frag_split) est gérée par lora_sync via lora_tx_packetize
     * — elle n'utilise pas ce pack mais sérialise directement en CBOR.
     */
    if (1 + cbor_len > COMM_MSG_LORA_MAX) {
        return -1;
    }

    *out_len = 1 + cbor_len;
    return 0;
}

/* ================================================================
 * Unpacking
 * ================================================================ */

int comm_msg_get_type(const uint8_t *buf, size_t buf_len,
                      comm_msg_type_t *type)
{
    if (!buf || buf_len == 0 || !type) {
        return -1;
    }

    uint8_t t = buf[0];

    /*
     * Vérifier que le type est connu.
     *
     * [F-CP-002] Cas particulier de COMM_MSG_LORA_FRAG (0x11) : ce
     * type n'a délibérément PAS de couple comm_msg_pack_frag /
     * comm_msg_unpack_frag ni de struct comm_msg_frag_t. C'est un
     * type radio bas-niveau dont le format wire est
     * [type:1][index:1][total:1][seq_id:1][payload:N] et qui est
     * géré directement par lora_sync (lora_frag_receive /
     * lora_frag_split / lora_tx_packetize) sans passer par le
     * sérialiseur CBOR. Sa présence ici sert uniquement à
     * permettre au dispatcher d'aiguiller le paquet vers le bon
     * case dans lora_sync_handle_rx — pas à signaler qu'une
     * fonction unpack_frag existe côté comm_protocol.
     */
    switch (t) {
        case COMM_MSG_DISCOVER:
        case COMM_MSG_ANNOUNCE:
        case COMM_MSG_TX_LOCKED:
        case COMM_MSG_TX_ACK:
        case COMM_MSG_LORA_TX:
        case COMM_MSG_LORA_FRAG: /* type radio bas-niveau, voir commentaire ci-dessus */
        case COMM_MSG_LORA_TIME_SYNC:
        case COMM_MSG_LORA_BROADCAST:
        case COMM_MSG_LORA_PING:
        case COMM_MSG_LORA_PONG:
        case COMM_MSG_LORA_SET_ALIAS:
        case COMM_MSG_LORA_SET_BENEFICIARY:
        case COMM_MSG_LORA_ATTESTATION: /* [Lot E.1bis] case oublie depuis l'ajout I2-fix */
            *type = (comm_msg_type_t)t;
            return 0;
        default:
            return -1;
    }
}

int comm_msg_unpack_discover(const uint8_t *buf, size_t buf_len,
                             comm_msg_discover_t *msg)
{
    /* Vérifier type + taille minimale (1 + 32 = 33) */
    if (!buf || !msg || buf_len < 33 || buf[0] != COMM_MSG_DISCOVER) {
        return -1;
    }

    memcpy(msg->sender_key.bytes, &buf[1], CRYPTO_PUBLIC_KEY_SIZE);
    return 0;
}

int comm_msg_unpack_announce(const uint8_t *buf, size_t buf_len,
                             comm_msg_announce_t *msg)
{
    /* Taille minimale : 1 (type) + 32 (pubkey) + 64 (sig) + 4 (nonce) + 1 (alias_len) = 102 */
    if (!buf || !msg || buf_len < COMM_MSG_ANNOUNCE_MIN_SIZE ||
        buf[0] != COMM_MSG_ANNOUNCE) {
        return -1;
    }

    size_t offset = 1; /* Sauter le type */

    /* Extraire la clé publique du device (32 octets) */
    memcpy(msg->device_key.bytes, &buf[offset], CRYPTO_PUBLIC_KEY_SIZE);
    offset += CRYPTO_PUBLIC_KEY_SIZE;

    /* Extraire la signature Ed25519 (64 octets) */
    memcpy(msg->signature.bytes, &buf[offset], CRYPTO_SIGNATURE_SIZE);
    offset += CRYPTO_SIGNATURE_SIZE;

    /* Extraire le nonce anti-rejeu (4 octets big-endian) */
    msg->nonce = read_u32_be(&buf[offset]);
    offset += 4;

    /* Extraire la longueur de l'alias */
    msg->alias_len = buf[offset++];

    /* Vérifier que l'alias ne dépasse pas le max */
    if (msg->alias_len > COMM_MSG_ALIAS_MAX) {
        return -1;
    }

    /* Vérifier que le buffer contient assez de données pour l'alias */
    if (buf_len < COMM_MSG_ANNOUNCE_MIN_SIZE + msg->alias_len) {
        return -1;
    }

    /*
     * Copier l'alias et terminer par null.
     * [F-CP-007] Cas alias_len == 0 : memcpy de taille 0 est un no-op
     * (comportement défini par la norme C99 §7.21.2.1), puis
     * msg->alias[0] = '\0' pose le terminateur en position 0 — le
     * buffer renvoyé est donc une chaîne vide valide ("\0"), pas
     * un buffer non initialisé. Conforme à F-CP-004.
     */
    memcpy(msg->alias, &buf[offset], msg->alias_len);
    msg->alias[msg->alias_len] = '\0';
    return 0;
}

int comm_msg_unpack_tx_locked(const uint8_t *buf, size_t buf_len,
                              transaction_t *tx)
{
    if (!buf || !tx || buf_len < 2 || buf[0] != COMM_MSG_TX_LOCKED) {
        return -1;
    }

    /* Désérialiser le CBOR à partir de buf[1] */
    esp_err_t err = tx_deserialize(&buf[1], buf_len - 1, tx);
    return (err == ESP_OK) ? 0 : -1;
}

int comm_msg_unpack_ack(const uint8_t *buf, size_t buf_len,
                        comm_msg_ack_t *msg)
{
    /* Taille fixe : 1 (type) + 32 (pubkey) + 64 (sig) + 4 (nonce) + 32 (tx_id) = 133 */
    if (!buf || !msg || buf_len < COMM_MSG_ACK_SIZE ||
        buf[0] != COMM_MSG_TX_ACK) {
        return -1;
    }

    size_t offset = 1; /* Sauter le type */

    /* Extraire la clé publique de l'émetteur (32 octets) */
    memcpy(msg->sender_key.bytes, &buf[offset], CRYPTO_PUBLIC_KEY_SIZE);
    offset += CRYPTO_PUBLIC_KEY_SIZE;

    /* Extraire la signature Ed25519 (64 octets) */
    memcpy(msg->signature.bytes, &buf[offset], CRYPTO_SIGNATURE_SIZE);
    offset += CRYPTO_SIGNATURE_SIZE;

    /* Extraire le nonce anti-rejeu (4 octets big-endian) */
    msg->nonce = read_u32_be(&buf[offset]);
    offset += 4;

    /* Extraire le hash de la transaction (32 octets) */
    memcpy(msg->tx_id.bytes, &buf[offset], CRYPTO_HASH_SIZE);

    return 0;
}

int comm_msg_unpack_lora_tx(const uint8_t *buf, size_t buf_len,
                            transaction_t *tx)
{
    if (!buf || !tx || buf_len < 2 || buf[0] != COMM_MSG_LORA_TX) {
        return -1;
    }

    esp_err_t err = tx_deserialize(&buf[1], buf_len - 1, tx);
    return (err == ESP_OK) ? 0 : -1;
}

/* ================================================================
 * LORA_TIME_SYNC — Packing / Unpacking
 * ================================================================ */

/**
 * Helper : écrire un uint16_t en big-endian dans un buffer.
 */
static void write_u16_be(uint8_t *dst, uint16_t val)
{
    dst[0] = (uint8_t)(val >> 8);
    dst[1] = (uint8_t)(val);
}

/**
 * Helper : lire un uint16_t en big-endian depuis un buffer.
 */
static uint16_t read_u16_be(const uint8_t *src)
{
    return ((uint16_t)src[0] << 8) | (uint16_t)src[1];
}

/**
 * Helper : écrire un uint32_t en big-endian dans un buffer.
 */
static void write_u32_be(uint8_t *dst, uint32_t val)
{
    dst[0] = (uint8_t)(val >> 24);
    dst[1] = (uint8_t)(val >> 16);
    dst[2] = (uint8_t)(val >> 8);
    dst[3] = (uint8_t)(val);
}

/**
 * Helper : lire un uint32_t en big-endian depuis un buffer.
 */
static uint32_t read_u32_be(const uint8_t *src)
{
    return ((uint32_t)src[0] << 24) |
           ((uint32_t)src[1] << 16) |
           ((uint32_t)src[2] << 8)  |
           ((uint32_t)src[3]);
}

/**
 * Helper : écrire un uint64_t en big-endian dans un buffer.
 */
static void write_u64_be(uint8_t *dst, uint64_t val)
{
    dst[0] = (uint8_t)(val >> 56);
    dst[1] = (uint8_t)(val >> 48);
    dst[2] = (uint8_t)(val >> 40);
    dst[3] = (uint8_t)(val >> 32);
    dst[4] = (uint8_t)(val >> 24);
    dst[5] = (uint8_t)(val >> 16);
    dst[6] = (uint8_t)(val >> 8);
    dst[7] = (uint8_t)(val);
}

/**
 * Helper : lire un uint64_t en big-endian depuis un buffer.
 */
static uint64_t read_u64_be(const uint8_t *src)
{
    return ((uint64_t)src[0] << 56) |
           ((uint64_t)src[1] << 48) |
           ((uint64_t)src[2] << 40) |
           ((uint64_t)src[3] << 32) |
           ((uint64_t)src[4] << 24) |
           ((uint64_t)src[5] << 16) |
           ((uint64_t)src[6] << 8)  |
           ((uint64_t)src[7]);
}

int comm_msg_pack_time_sync(uint8_t *buf, size_t buf_len,
                            const public_key_t *master_key,
                            const signature_t *sig,
                            uint64_t master_timestamp,
                            uint64_t master_lamport,
                            size_t *out_len)
{
    if (!buf || !master_key || !sig || !out_len ||
        buf_len < COMM_MSG_TIME_SYNC_SIZE) {
        return -1;
    }

    /*
     * Format : [0x12][pubkey:32][sig:64][timestamp:8 BE][lamport:8 BE]
     * Taille totale : 113 octets (fixe)
     */
    size_t offset = 0;

    buf[offset++] = COMM_MSG_LORA_TIME_SYNC;                          /* type : 1 octet */
    memcpy(&buf[offset], master_key->bytes, CRYPTO_PUBLIC_KEY_SIZE);   /* pubkey : 32 octets */
    offset += CRYPTO_PUBLIC_KEY_SIZE;
    memcpy(&buf[offset], sig->bytes, CRYPTO_SIGNATURE_SIZE);           /* sig : 64 octets */
    offset += CRYPTO_SIGNATURE_SIZE;
    write_u64_be(&buf[offset], master_timestamp);                      /* timestamp : 8 octets BE */
    offset += 8;
    write_u64_be(&buf[offset], master_lamport);                        /* lamport : 8 octets BE */
    offset += 8;

    *out_len = offset;
    return 0;
}

int comm_msg_unpack_time_sync(const uint8_t *buf, size_t buf_len,
                              comm_msg_time_sync_t *msg)
{
    if (!buf || !msg || buf_len < COMM_MSG_TIME_SYNC_SIZE ||
        buf[0] != COMM_MSG_LORA_TIME_SYNC) {
        return -1;
    }

    size_t offset = 1; /* Sauter le type */

    /* Extraire la clé publique du maître (32 octets) */
    memcpy(msg->master_key.bytes, &buf[offset], CRYPTO_PUBLIC_KEY_SIZE);
    offset += CRYPTO_PUBLIC_KEY_SIZE;

    /* Extraire la signature Ed25519 (64 octets) */
    memcpy(msg->signature.bytes, &buf[offset], CRYPTO_SIGNATURE_SIZE);
    offset += CRYPTO_SIGNATURE_SIZE;

    /* Extraire le timestamp et la valeur Lamport (8 octets BE chacun) */
    msg->master_timestamp = read_u64_be(&buf[offset]);
    offset += 8;
    msg->master_lamport = read_u64_be(&buf[offset]);

    return 0;
}

/* ================================================================
 * LORA_BROADCAST — Packing / Unpacking
 * ================================================================ */

int comm_msg_pack_broadcast(uint8_t *buf, size_t buf_len,
                            const public_key_t *sender_key,
                            const signature_t *sig,
                            const char *text, uint8_t text_len,
                            size_t *out_len)
{
    if (!buf || !sender_key || !sig || !text || !out_len) {
        return -1;
    }

    /* text_len doit être entre 1 et COMM_MSG_BROADCAST_TEXT_MAX */
    if (text_len == 0 || text_len > COMM_MSG_BROADCAST_TEXT_MAX) {
        return -1;
    }

    /*
     * Format : [0x13][pubkey:32][sig:64][text_len:1][text:N]
     * Taille totale : 1 + 32 + 64 + 1 + N = 98 + N
     */
    size_t total = COMM_MSG_BROADCAST_MIN_SIZE + text_len;
    if (buf_len < total) {
        return -1;
    }

    size_t offset = 0;

    buf[offset++] = COMM_MSG_LORA_BROADCAST;                      /* type : 1 octet */
    memcpy(&buf[offset], sender_key->bytes, CRYPTO_PUBLIC_KEY_SIZE); /* pubkey : 32 octets */
    offset += CRYPTO_PUBLIC_KEY_SIZE;
    memcpy(&buf[offset], sig->bytes, CRYPTO_SIGNATURE_SIZE);         /* sig : 64 octets */
    offset += CRYPTO_SIGNATURE_SIZE;
    buf[offset++] = text_len;                                        /* text_len : 1 octet */
    memcpy(&buf[offset], text, text_len);                            /* text : N octets */
    offset += text_len;

    *out_len = offset;
    return 0;
}

int comm_msg_unpack_broadcast(const uint8_t *buf, size_t buf_len,
                              comm_msg_broadcast_t *msg)
{
    /* Vérifier type + taille minimale (98 = type + pubkey + sig + text_len) */
    if (!buf || !msg || buf_len < COMM_MSG_BROADCAST_MIN_SIZE ||
        buf[0] != COMM_MSG_LORA_BROADCAST) {
        return -1;
    }

    size_t offset = 1; /* Sauter le type */

    /* Extraire la clé publique du maître (32 octets) */
    memcpy(msg->sender_key.bytes, &buf[offset], CRYPTO_PUBLIC_KEY_SIZE);
    offset += CRYPTO_PUBLIC_KEY_SIZE;

    /* Extraire la signature Ed25519 (64 octets) */
    memcpy(msg->signature.bytes, &buf[offset], CRYPTO_SIGNATURE_SIZE);
    offset += CRYPTO_SIGNATURE_SIZE;

    /* Extraire la longueur du texte */
    msg->text_len = buf[offset++];

    /* Valider la longueur du texte */
    if (msg->text_len == 0 || msg->text_len > COMM_MSG_BROADCAST_TEXT_MAX) {
        return -1;
    }

    /* Vérifier que le buffer contient assez de données pour le texte */
    if (buf_len < COMM_MSG_BROADCAST_MIN_SIZE + msg->text_len) {
        return -1;
    }

    /* Copier le texte et terminer par null */
    memcpy(msg->text, &buf[offset], msg->text_len);
    msg->text[msg->text_len] = '\0';

    return 0;
}

/* ================================================================
 * LORA_PING / LORA_PONG — Packing / Unpacking
 * ================================================================ */

int comm_msg_pack_ping(uint8_t *buf, size_t buf_len,
                       const public_key_t *master_key,
                       const signature_t *sig,
                       uint16_t ping_id, size_t *out_len)
{
    if (!buf || !master_key || !sig || !out_len ||
        buf_len < COMM_MSG_PING_SIZE) {
        return -1;
    }

    /*
     * Format : [0x14][master_pubkey:32][sig:64][ping_id:2 BE]
     * Taille totale : 99 octets (fixe)
     */
    size_t offset = 0;

    buf[offset++] = COMM_MSG_LORA_PING;                               /* type : 1 octet */
    memcpy(&buf[offset], master_key->bytes, CRYPTO_PUBLIC_KEY_SIZE);   /* pubkey : 32 octets */
    offset += CRYPTO_PUBLIC_KEY_SIZE;
    memcpy(&buf[offset], sig->bytes, CRYPTO_SIGNATURE_SIZE);           /* sig : 64 octets */
    offset += CRYPTO_SIGNATURE_SIZE;
    write_u16_be(&buf[offset], ping_id);                               /* ping_id : 2 octets BE */
    offset += 2;

    *out_len = offset;
    return 0;
}

int comm_msg_unpack_ping(const uint8_t *buf, size_t buf_len,
                         comm_msg_ping_t *msg)
{
    if (!buf || !msg || buf_len < COMM_MSG_PING_SIZE ||
        buf[0] != COMM_MSG_LORA_PING) {
        return -1;
    }

    size_t offset = 1; /* Sauter le type */

    /* Extraire la clé publique du maître (32 octets) */
    memcpy(msg->master_key.bytes, &buf[offset], CRYPTO_PUBLIC_KEY_SIZE);
    offset += CRYPTO_PUBLIC_KEY_SIZE;

    /* Extraire la signature Ed25519 (64 octets) */
    memcpy(msg->signature.bytes, &buf[offset], CRYPTO_SIGNATURE_SIZE);
    offset += CRYPTO_SIGNATURE_SIZE;

    /* Extraire le ping_id (2 octets big-endian) */
    msg->ping_id = read_u16_be(&buf[offset]);

    return 0;
}

int comm_msg_pack_pong(uint8_t *buf, size_t buf_len,
                       const public_key_t *device_key,
                       const signature_t *sig,
                       uint16_t ping_id,
                       const char *alias, uint8_t alias_len,
                       size_t *out_len)
{
    if (!buf || !device_key || !sig || !alias || !out_len) {
        return -1;
    }

    /*
     * [F-CP-005] alias_len == 0 est LÉGAL pour PONG (symétrique
     * avec pack_announce, cf. F-CP-004) : un device sans alias
     * répond à un PING avec sa clé publique nue. Le test
     * pong_roundtrip_empty_alias documente l'intention. Les couches
     * supérieures qui exigent un alias non-vide (UI de scan,
     * rename, forward) doivent valider sémantiquement en amont.
     */

    /* Tronquer l'alias si trop long */
    if (alias_len > COMM_MSG_ALIAS_MAX) {
        alias_len = COMM_MSG_ALIAS_MAX;
    }

    /* [0x15][device_pubkey:32][sig:64][ping_id:2 BE][alias_len:1][alias:N] */
    size_t total = COMM_MSG_PONG_MIN_SIZE + alias_len;
    if (buf_len < total) {
        return -1;
    }

    size_t offset = 0;
    buf[offset++] = COMM_MSG_LORA_PONG;
    memcpy(&buf[offset], device_key->bytes, CRYPTO_PUBLIC_KEY_SIZE);
    offset += CRYPTO_PUBLIC_KEY_SIZE;
    memcpy(&buf[offset], sig->bytes, CRYPTO_SIGNATURE_SIZE);
    offset += CRYPTO_SIGNATURE_SIZE;
    write_u16_be(&buf[offset], ping_id);
    offset += 2;
    buf[offset++] = alias_len;
    memcpy(&buf[offset], alias, alias_len);
    offset += alias_len;

    *out_len = offset;
    return 0;
}

int comm_msg_unpack_pong(const uint8_t *buf, size_t buf_len,
                         comm_msg_pong_t *msg)
{
    if (!buf || !msg || buf_len < COMM_MSG_PONG_MIN_SIZE ||
        buf[0] != COMM_MSG_LORA_PONG) {
        return -1;
    }

    size_t offset = 1;

    /* Extraire la cle publique du device (32 octets) */
    memcpy(msg->device_key.bytes, &buf[offset], CRYPTO_PUBLIC_KEY_SIZE);
    offset += CRYPTO_PUBLIC_KEY_SIZE;

    /* Extraire la signature Ed25519 (64 octets) */
    memcpy(msg->signature.bytes, &buf[offset], CRYPTO_SIGNATURE_SIZE);
    offset += CRYPTO_SIGNATURE_SIZE;

    /* Extraire le ping_id (2 octets big-endian) */
    msg->ping_id = read_u16_be(&buf[offset]);
    offset += 2;

    msg->alias_len = buf[offset++];

    if (msg->alias_len > COMM_MSG_ALIAS_MAX) {
        return -1;
    }
    if (buf_len < COMM_MSG_PONG_MIN_SIZE + msg->alias_len) {
        return -1;
    }

    /*
     * Copier l'alias et terminer par null.
     * [F-CP-007] Cas alias_len == 0 (légal, cf. F-CP-005) :
     * memcpy de taille 0 est un no-op et msg->alias[0] = '\0' pose
     * le terminateur en position 0. Le buffer renvoyé est une chaîne
     * vide valide.
     */
    memcpy(msg->alias, &buf[offset], msg->alias_len);
    msg->alias[msg->alias_len] = '\0';

    return 0;
}

/* ================================================================
 * LORA_ATTESTATION — Packing / Unpacking [I2-fix]
 * ================================================================ */

int comm_msg_pack_attestation(uint8_t *buf, size_t buf_len,
                              const public_key_t *attester_key,
                              const signature_t *sig,
                              const hash_t *tx_id,
                              size_t *out_len)
{
    if (!buf || !attester_key || !sig || !tx_id || !out_len ||
        buf_len < COMM_MSG_ATTESTATION_SIZE) {
        return -1;
    }

    /* Format : [0x18][attester_pubkey:32][sig:64][tx_id:32] */
    size_t offset = 0;
    buf[offset++] = COMM_MSG_LORA_ATTESTATION;
    memcpy(&buf[offset], attester_key->bytes, CRYPTO_PUBLIC_KEY_SIZE);
    offset += CRYPTO_PUBLIC_KEY_SIZE;
    memcpy(&buf[offset], sig->bytes, CRYPTO_SIGNATURE_SIZE);
    offset += CRYPTO_SIGNATURE_SIZE;
    memcpy(&buf[offset], tx_id->bytes, CRYPTO_HASH_SIZE);
    offset += CRYPTO_HASH_SIZE;

    *out_len = offset;
    return 0;
}

int comm_msg_unpack_attestation(const uint8_t *buf, size_t buf_len,
                                comm_msg_attestation_t *msg)
{
    if (!buf || !msg || buf_len < COMM_MSG_ATTESTATION_SIZE ||
        buf[0] != COMM_MSG_LORA_ATTESTATION) {
        return -1;
    }

    size_t offset = 1;
    memcpy(msg->attester_key.bytes, &buf[offset], CRYPTO_PUBLIC_KEY_SIZE);
    offset += CRYPTO_PUBLIC_KEY_SIZE;
    memcpy(msg->signature.bytes, &buf[offset], CRYPTO_SIGNATURE_SIZE);
    offset += CRYPTO_SIGNATURE_SIZE;
    memcpy(msg->tx_id.bytes, &buf[offset], CRYPTO_HASH_SIZE);

    return 0;
}

/* ================================================================
 * LORA_SET_ALIAS — Packing / Unpacking
 * ================================================================ */

int comm_msg_pack_set_alias(uint8_t *buf, size_t buf_len,
                            const public_key_t *master_key,
                            const signature_t *sig,
                            const public_key_t *target_key,
                            const char *alias, uint8_t alias_len,
                            size_t *out_len)
{
    if (!buf || !master_key || !sig || !target_key || !alias || !out_len) {
        return -1;
    }

    /* alias_len doit être entre 1 et COMM_MSG_ALIAS_MAX */
    if (alias_len == 0 || alias_len > COMM_MSG_ALIAS_MAX) {
        return -1;
    }

    /*
     * Format : [0x16][master_pubkey:32][sig:64][target_pubkey:32][alias_len:1][alias:N]
     * Taille totale : 1 + 32 + 64 + 32 + 1 + N = 130 + N
     */
    size_t total = COMM_MSG_SET_ALIAS_MIN_SIZE + alias_len;
    if (buf_len < total) {
        return -1;
    }

    size_t offset = 0;

    buf[offset++] = COMM_MSG_LORA_SET_ALIAS;                            /* type : 1 octet */
    memcpy(&buf[offset], master_key->bytes, CRYPTO_PUBLIC_KEY_SIZE);     /* master_key : 32 octets */
    offset += CRYPTO_PUBLIC_KEY_SIZE;
    memcpy(&buf[offset], sig->bytes, CRYPTO_SIGNATURE_SIZE);             /* sig : 64 octets */
    offset += CRYPTO_SIGNATURE_SIZE;
    memcpy(&buf[offset], target_key->bytes, CRYPTO_PUBLIC_KEY_SIZE);     /* target_key : 32 octets */
    offset += CRYPTO_PUBLIC_KEY_SIZE;
    buf[offset++] = alias_len;                                           /* alias_len : 1 octet */
    memcpy(&buf[offset], alias, alias_len);                              /* alias : N octets */
    offset += alias_len;

    *out_len = offset;
    return 0;
}

int comm_msg_unpack_set_alias(const uint8_t *buf, size_t buf_len,
                              comm_msg_set_alias_t *msg)
{
    /* Vérifier type + taille minimale (130 = type + master_key + sig + target_key + alias_len) */
    if (!buf || !msg || buf_len < COMM_MSG_SET_ALIAS_MIN_SIZE ||
        buf[0] != COMM_MSG_LORA_SET_ALIAS) {
        return -1;
    }

    size_t offset = 1; /* Sauter le type */

    /* Extraire la clé publique du maître (32 octets) */
    memcpy(msg->master_key.bytes, &buf[offset], CRYPTO_PUBLIC_KEY_SIZE);
    offset += CRYPTO_PUBLIC_KEY_SIZE;

    /* Extraire la signature Ed25519 (64 octets) */
    memcpy(msg->signature.bytes, &buf[offset], CRYPTO_SIGNATURE_SIZE);
    offset += CRYPTO_SIGNATURE_SIZE;

    /* Extraire la clé publique du device cible (32 octets) */
    memcpy(msg->target_key.bytes, &buf[offset], CRYPTO_PUBLIC_KEY_SIZE);
    offset += CRYPTO_PUBLIC_KEY_SIZE;

    /* Extraire la longueur de l'alias */
    msg->alias_len = buf[offset++];

    /* Valider la longueur de l'alias */
    if (msg->alias_len == 0 || msg->alias_len > COMM_MSG_ALIAS_MAX) {
        return -1;
    }

    /* Vérifier que le buffer contient assez de données pour l'alias */
    if (buf_len < COMM_MSG_SET_ALIAS_MIN_SIZE + msg->alias_len) {
        return -1;
    }

    /* Copier l'alias et terminer par null */
    memcpy(msg->alias, &buf[offset], msg->alias_len);
    msg->alias[msg->alias_len] = '\0';

    return 0;
}

/* ================================================================
 * LORA_SET_BENEFICIARY — Packing / Unpacking
 * ================================================================ */

int comm_msg_pack_set_beneficiary(uint8_t *buf, size_t buf_len,
                                  const public_key_t *master_key,
                                  const signature_t *sig,
                                  const public_key_t *target_key,
                                  const public_key_t *beneficiary_key,
                                  uint16_t forward_interval_min,
                                  size_t *out_len)
{
    if (!buf || !master_key || !sig || !target_key || !beneficiary_key || !out_len) {
        return -1;
    }

    if (buf_len < COMM_MSG_SET_BENEFICIARY_SIZE) {
        return -1;
    }

    /*
     * Format : [0x17][master_key:32][sig:64][target_key:32][beneficiary_key:32][interval:2 BE]
     * Taille totale : 163 octets (fixe)
     */
    size_t offset = 0;

    buf[offset++] = COMM_MSG_LORA_SET_BENEFICIARY;                       /* type : 1 octet */
    memcpy(&buf[offset], master_key->bytes, CRYPTO_PUBLIC_KEY_SIZE);      /* master_key : 32 */
    offset += CRYPTO_PUBLIC_KEY_SIZE;
    memcpy(&buf[offset], sig->bytes, CRYPTO_SIGNATURE_SIZE);              /* sig : 64 */
    offset += CRYPTO_SIGNATURE_SIZE;
    memcpy(&buf[offset], target_key->bytes, CRYPTO_PUBLIC_KEY_SIZE);      /* target_key : 32 */
    offset += CRYPTO_PUBLIC_KEY_SIZE;
    memcpy(&buf[offset], beneficiary_key->bytes, CRYPTO_PUBLIC_KEY_SIZE); /* beneficiary_key : 32 */
    offset += CRYPTO_PUBLIC_KEY_SIZE;
    write_u16_be(&buf[offset], forward_interval_min);                     /* interval : 2 BE */
    offset += 2;

    *out_len = offset;
    return 0;
}

int comm_msg_unpack_set_beneficiary(const uint8_t *buf, size_t buf_len,
                                    comm_msg_set_beneficiary_t *msg)
{
    if (!buf || !msg || buf_len < COMM_MSG_SET_BENEFICIARY_SIZE ||
        buf[0] != COMM_MSG_LORA_SET_BENEFICIARY) {
        return -1;
    }

    size_t offset = 1; /* Sauter le type */

    /* Clé publique du maître (32 octets) */
    memcpy(msg->master_key.bytes, &buf[offset], CRYPTO_PUBLIC_KEY_SIZE);
    offset += CRYPTO_PUBLIC_KEY_SIZE;

    /* Signature Ed25519 (64 octets) */
    memcpy(msg->signature.bytes, &buf[offset], CRYPTO_SIGNATURE_SIZE);
    offset += CRYPTO_SIGNATURE_SIZE;

    /* Clé publique du device cible (32 octets) */
    memcpy(msg->target_key.bytes, &buf[offset], CRYPTO_PUBLIC_KEY_SIZE);
    offset += CRYPTO_PUBLIC_KEY_SIZE;

    /* Clé publique du bénéficiaire (32 octets) */
    memcpy(msg->beneficiary_key.bytes, &buf[offset], CRYPTO_PUBLIC_KEY_SIZE);
    offset += CRYPTO_PUBLIC_KEY_SIZE;

    /* Intervalle de forward (2 octets big-endian) */
    msg->forward_interval_min = read_u16_be(&buf[offset]);

    return 0;
}

/* ================================================================
 * Verification de signatures [Lot B item 4]
 *
 * Ces fonctions reconstituent le buffer canonical signe pour chaque
 * type de message LoRa puis appellent crypto_verify(). Elles existent
 * pour permettre a la couche LoRa de filtrer les messages mal signes
 * avant de les poster dans la queue d'evenements core_task (defense
 * en profondeur : core_task continue de faire ses propres verifs
 * d'autorite/identite).
 *
 * Convention de retour : 0 si signature valide, -1 sinon (identique
 * aux fonctions pack/unpack du module).
 * ================================================================ */

int comm_msg_verify_broadcast(const comm_msg_broadcast_t *msg)
{
    if (!msg) return -1;
    if (msg->text_len == 0 || msg->text_len > COMM_MSG_BROADCAST_TEXT_MAX) {
        return -1;
    }

    /*
     * Format signe : [text_len:1][text:N]. Doit etre identique au
     * buffer que l'emetteur a signe (cf. struct comm_msg_broadcast_t
     * dans comm_msg.h et le handler dans main.c handle_broadcast_received).
     */
    uint8_t signed_buf[1 + COMM_MSG_BROADCAST_TEXT_MAX];
    signed_buf[0] = msg->text_len;
    memcpy(&signed_buf[1], msg->text, msg->text_len);

    esp_err_t err = crypto_verify(signed_buf, 1 + msg->text_len,
                                   &msg->sender_key, &msg->signature);
    return (err == ESP_OK) ? 0 : -1;
}

int comm_msg_verify_set_alias(const comm_msg_set_alias_t *msg)
{
    if (!msg) return -1;
    if (msg->alias_len == 0 || msg->alias_len > COMM_MSG_ALIAS_MAX) {
        return -1;
    }

    /*
     * Format signe : [target_key:32][alias_len:1][alias:N].
     * Verifie avec la cle publique du maitre (msg->master_key) :
     * l'authentification de l'identite "maitre autorise" reste
     * a la charge de core_task (mint_authorities).
     */
    uint8_t signed_buf[CRYPTO_PUBLIC_KEY_SIZE + 1 + COMM_MSG_ALIAS_MAX];
    size_t  signed_len = 0;
    memcpy(&signed_buf[signed_len], msg->target_key.bytes, CRYPTO_PUBLIC_KEY_SIZE);
    signed_len += CRYPTO_PUBLIC_KEY_SIZE;
    signed_buf[signed_len++] = msg->alias_len;
    memcpy(&signed_buf[signed_len], msg->alias, msg->alias_len);
    signed_len += msg->alias_len;

    esp_err_t err = crypto_verify(signed_buf, signed_len,
                                   &msg->master_key, &msg->signature);
    return (err == ESP_OK) ? 0 : -1;
}

int comm_msg_verify_set_beneficiary(const comm_msg_set_beneficiary_t *msg)
{
    if (!msg) return -1;

    /*
     * Format signe : [target_key:32][beneficiary_key:32][interval:2 BE].
     * Verifie avec la cle publique du maitre (msg->master_key) ;
     * l'authentification du maitre comme autorite reste a core_task.
     */
    uint8_t signed_buf[CRYPTO_PUBLIC_KEY_SIZE * 2 + 2];
    size_t  signed_len = 0;
    memcpy(&signed_buf[signed_len], msg->target_key.bytes, CRYPTO_PUBLIC_KEY_SIZE);
    signed_len += CRYPTO_PUBLIC_KEY_SIZE;
    memcpy(&signed_buf[signed_len], msg->beneficiary_key.bytes,
           CRYPTO_PUBLIC_KEY_SIZE);
    signed_len += CRYPTO_PUBLIC_KEY_SIZE;
    write_u16_be(&signed_buf[signed_len], msg->forward_interval_min);
    signed_len += 2;

    esp_err_t err = crypto_verify(signed_buf, signed_len,
                                   &msg->master_key, &msg->signature);
    return (err == ESP_OK) ? 0 : -1;
}

int comm_msg_verify_attestation(const comm_msg_attestation_t *msg)
{
    if (!msg) return -1;

    /*
     * [F-CP-003] Format signe : [tx_id:32] (cf. comm_msg.h:225-228).
     * Verifie avec la cle publique de l'attestant (msg->attester_key).
     * La validation supplementaire "attester_key == tx.to" reste a la
     * charge de core_task qui a acces au DAG.
     */
    esp_err_t err = crypto_verify(msg->tx_id.bytes, CRYPTO_HASH_SIZE,
                                   &msg->attester_key, &msg->signature);
    return (err == ESP_OK) ? 0 : -1;
}
