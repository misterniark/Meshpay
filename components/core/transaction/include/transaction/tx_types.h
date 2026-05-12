/**
 * @file tx_types.h
 * @brief Types et structures fondamentaux pour les transactions.
 *
 * Définit la structure centrale transaction_t qui est le noeud de base
 * du DAG. Chaque transaction représente un transfert de crédits entre
 * deux devices ou une création de crédits par un device maître (MINT).
 *
 * Contrainte majeure : la transaction sérialisée en CBOR doit tenir
 * dans 320 octets (TX_CBOR_MAX_SIZE). ESP-NOW V1 plafonnait à 250 octets ;
 * le projet utilise ESP-NOW V2, ce qui a permis de monter à 320 depuis le
 * Lot E.1bis afin d'accommoder le champ `seq` ajouté pour l'anti
 * double-dépense [I3-fix].
 */

#ifndef TX_TYPES_H
#define TX_TYPES_H

#include "crypto/crypto_types.h"
#include <stdint.h>

/**
 * Nombre maximum de parents par transaction dans le DAG.
 * Limité à 2 pour respecter la contrainte de 320 octets en CBOR
 * (TX_CBOR_MAX_SIZE, ESP-NOW V2 depuis Lot E.1bis ; auparavant 250 en V1) :
 * chaque parent coûte 32 octets (hash SHA-256).
 */
#define TX_MAX_PARENTS 2

/**
 * @brief Mapping des clés CBOR pour la sérialisation.
 *
 * On utilise des clés numériques (entiers) au lieu de chaînes de caractères
 * pour minimiser la taille sérialisée. Avec des clés texte ("id", "type"...),
 * on dépasserait les 320 octets (TX_CBOR_MAX_SIZE, ESP-NOW V2 depuis Lot E.1bis,
 * 250 octets sous ESP-NOW V1). Les entiers ne coûtent qu'1 octet chacun en CBOR.
 */
#define CBOR_KEY_TYPE       1
#define CBOR_KEY_FROM       2
#define CBOR_KEY_TO         3
#define CBOR_KEY_AMOUNT     4
#define CBOR_KEY_PARENTS    5
#define CBOR_KEY_TIMESTAMP  6
/*
 * Clés 7-9 : champs de l'enveloppe complète UNIQUEMENT.
 * Elles ne font pas partie du payload signable (cf. tx_serialize_signable) :
 * la signature et le statut sont posés après calcul du hash, et l'id est
 * justement ce hash. On les regroupe ici pour garder une vue unique du
 * mapping CBOR, mais elles sont absentes du buffer hashé.
 */
#define CBOR_KEY_ID         7
#define CBOR_KEY_SIGNATURE  8
#define CBOR_KEY_STATUS     9
#define CBOR_KEY_CURRENCY   10
#define CBOR_KEY_FEE        11
/* [I3-fix] Nonce monotone par emetteur, pour la detection des conflits
 * de double-depense. Chaque emetteur incremente sa propre sequence a
 * chaque TX emise (TRANSFER ou MINT). Deux TX avec meme (from, seq) mais
 * id different sont en conflit. */
#define CBOR_KEY_SEQ        12

/** @brief Type de transaction. */
typedef enum {
    TX_TYPE_TRANSFER = 0,  /**< Transfert de crédits entre deux devices */
    TX_TYPE_MINT     = 1   /**< Création de crédits par un device maître */
} tx_type_t;

/** @brief Statut de la transaction dans le flux de paiement. */
typedef enum {
    TX_STATUS_LOCKED    = 0,  /**< Montant verrouillé, en attente d'ACK */
    TX_STATUS_CONFIRMED = 1,  /**< Paiement confirmé par le destinataire */
    TX_STATUS_CANCELLED = 2   /**< Annulé (timeout ou rejet) */
} tx_status_t;

/**
 * @brief Structure d'une transaction dans le DAG.
 *
 * Chaque transaction est un noeud du graphe acyclique dirigé.
 * Elle référence 1 ou 2 transactions parentes via leurs hashes.
 *
 * Taille mémoire estimée : ~233 octets (sans padding).
 *
 * Pour la sérialisation CBOR (contrainte 320 octets ESP-NOW V2 depuis
 * Lot E.1bis, héritage de 250 octets sous ESP-NOW V1), seuls les champs
 * "signable" sont sérialisés (tout sauf id et signature).
 * Le hash (id) et la signature sont calculés sur ce contenu sérialisé.
 */
typedef struct {
    hash_t      id;          /**< Hash SHA-256 du contenu sérialisé (= identifiant unique) */
    tx_type_t   type;        /**< TRANSFER ou MINT */
    public_key_t from;       /**< Clé publique de l'émetteur (vide si MINT) */
    public_key_t to;         /**< Clé publique du destinataire */
    uint32_t    amount;      /**< Montant en crédits (unité abstraite) */
    uint32_t    currency_id; /**< Identifiant de la monnaie (hash tronqué du manifeste) */
    uint32_t    fee;         /**< Frais de transfert brûlés au moment de la création (0 pour MINT) */
    uint32_t    seq;         /**< [I3-fix] Nonce monotone par emetteur — anti double-depense */
    hash_t      parents[TX_MAX_PARENTS]; /**< Hashes des transactions parentes */
    uint8_t     parent_count;/**< Nombre de parents effectifs (1 ou 2) */
    uint64_t    timestamp;   /**< Horodatage en millisecondes (epoch device) */
    signature_t signature;   /**< Signature Ed25519 de l'émetteur */
    tx_status_t status;      /**< LOCKED, CONFIRMED ou CANCELLED */
} transaction_t;

#endif /* TX_TYPES_H */
