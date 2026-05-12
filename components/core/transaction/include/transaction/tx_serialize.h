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
 * Les champs sérialisés sont : type, from, to, amount, parents, timestamp.
 * Les champs id et signature sont exclus car ils sont calculés à partir
 * de cette sérialisation.
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
 * @param[in]  buffer     Buffer CBOR d'entrée
 * @param[in]  buffer_len Taille du buffer
 * @param[out] tx         Transaction reconstruite
 * @return ESP_OK en cas de succès
 *         ESP_ERR_INVALID_ARG si le CBOR est malformé ou incomplet
 */
esp_err_t tx_deserialize(const uint8_t *buffer, size_t buffer_len,
                         transaction_t *tx);

#endif /* TX_SERIALIZE_H */
