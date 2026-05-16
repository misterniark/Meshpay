/**
 * @file tx_serialize.h
 * @brief Sérialisation et désérialisation CBOR des transactions.
 *
 * Deux modes de sérialisation :
 * - "signable" : tous les champs sauf id et signature.
 *   Utilisé pour calculer le hash (id) et la signature.
 * - "complète" : tous les champs inclus.
 *   Utilisé pour la transmission ESP-NOW et la sync LoRa.
 *
 * Les clés CBOR sont numériques (pas de chaînes) pour minimiser la taille.
 * Voir CBOR_KEY_* dans tx_types.h pour le mapping.
 *
 * Contrainte : la sérialisation complète doit tenir dans TX_CBOR_MAX_SIZE
 * octets. Cette limite est calee sur la taille de payload utile en ESP-NOW
 * V2 (depuis ESP-IDF 4.4, jusqu'a 1490 octets), avec une marge pour le
 * pire cas pathologique mesure (transfer 2 parents + montant 999999 +
 * expiration UINT64_MAX ~= 301 octets).
 *
 * Historique :
 *   - Avant Lot B : 250 octets (ESP-NOW V1 strict).
 *   - Lot E.1bis  : 320 octets. L'ajout du champ `seq` (Lot B) a pousse
 *     le pire cas au-dela de 250 ; on remonte le plafond a 320 plutot
 *     que de compacter le wire format.
 */

#ifndef TX_SERIALIZE_H
#define TX_SERIALIZE_H

#include "transaction/tx_types.h"
#include "esp_err.h"
#include <stddef.h>

/** Taille maximale d'une transaction sérialisée en CBOR.
 *  Voir le commentaire de tete pour la justification du choix 320. */
#define TX_CBOR_MAX_SIZE 320

/**
 * @brief Sérialise les champs "signables" d'une transaction en CBOR.
 *
 * [F-TX-004] Les 9 champs sérialisés sont : `type, from, to, amount,
 * parents, timestamp, currency_id, fee, seq`. Les champs `id`,
 * `signature` et `status` sont exclus : `id` est le SHA-256 du
 * signable lui-même, `signature` est l'Ed25519 de ce hash, et `status`
 * est volontairement non-signé (cf. politique CBOR_KEY_STATUS dans
 * tx_serialize.c).
 *
 * Une modification quelconque de l'un de ces 9 champs après signature
 * casse la vérification — toute la surface couverte par la signature
 * est listée ci-dessus.
 *
 * @param[in]  tx         Transaction à sérialiser
 * @param[out] buffer     Buffer de sortie CBOR
 * @param[in]  buffer_len Taille du buffer de sortie
 * @param[out] out_len    Nombre d'octets écrits dans le buffer
 * @return ESP_OK en cas de succès
 *         ESP_ERR_NO_MEM si le buffer est trop petit
 */
esp_err_t tx_serialize_signable(const transaction_t *tx,
                                uint8_t *buffer, size_t buffer_len,
                                size_t *out_len);

/**
 * @brief Sérialise une transaction complète en CBOR (incluant id, signature, status).
 *
 * Utilisé pour transmettre une transaction via ESP-NOW ou LoRa.
 * Le résultat doit tenir dans TX_CBOR_MAX_SIZE octets.
 *
 * @param[in]  tx         Transaction à sérialiser
 * @param[out] buffer     Buffer de sortie CBOR
 * @param[in]  buffer_len Taille du buffer de sortie
 * @param[out] out_len    Nombre d'octets écrits dans le buffer
 * @return ESP_OK en cas de succès
 *         ESP_ERR_NO_MEM si le buffer est trop petit
 */
esp_err_t tx_serialize_full(const transaction_t *tx,
                            uint8_t *buffer, size_t buffer_len,
                            size_t *out_len);

/**
 * @brief Désérialise une transaction complète depuis un buffer CBOR.
 *
 * Reconstruit une structure transaction_t à partir du CBOR reçu via
 * ESP-NOW ou LoRa. Tous les champs sont restaurés, y compris id,
 * signature et status.
 *
 * [F-TX-006] Cette fonction NE VALIDE PAS la structure ni la signature.
 * Tout appelant doit enchaîner :
 *   `tx_deserialize → tx_validate_structure → tx_validate_signature`
 * avant d'utiliser la transaction. Un CBOR tronqué donne une TX avec
 * des champs à zéro qui sera rejetée par `tx_validate_structure`.
 *
 * @param[in]  buffer     Buffer CBOR d'entrée
 * @param[in]  buffer_len Taille du buffer
 * @param[out] tx         Transaction reconstruite
 * @return ESP_OK en cas de succès
 *         ESP_ERR_INVALID_ARG si le CBOR est malformé ou incomplet
 */
esp_err_t tx_deserialize(const uint8_t *buffer, size_t buffer_len,
                         transaction_t *tx);

#endif /* TX_SERIALIZE_H */
