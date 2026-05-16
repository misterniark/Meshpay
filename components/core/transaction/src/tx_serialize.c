/**
 * @file tx_serialize.c
 * @brief Implémentation de la sérialisation/désérialisation CBOR des transactions.
 *
 * Utilise tinycbor pour encoder/décoder les transactions en format CBOR.
 * Les clés de la map CBOR sont des entiers (pas des chaînes) pour
 * minimiser la taille. Voir CBOR_KEY_* dans tx_types.h.
 *
 * Budget d'octets pour une TX TRANSFER avec 2 parents :
 * - Map header           : ~3 octets (map de 6 ou 9 entrées)
 * - type (1 octet clé)   : 2 octets (clé + valeur enum)
 * - from (32 octets)     : 35 octets (clé + header bstr + données)
 * - to (32 octets)       : 35 octets
 * - amount (uint32)      : 6 octets max (clé + uint32)
 * - parents (2x32 oct)   : 70 octets (clé + array header + 2 bstr)
 * - timestamp (uint64)   : 10 octets max (clé + uint64)
 * Total signable         : ~161 octets
 * + id (32 octets)       : 35 octets
 * + signature (64 octets): 67 octets
 * + status               : 2 octets
 * Total complet          : ~265 octets — trop !
 *
 * Solution : pour la transmission ESP-NOW, on envoie la version "signable"
 * + id + signature séparément, ou on optimise l'encodage.
 * Actuellement on sérialise en "full" avec toutes les données et on
 * vérifie la taille à l'exécution.
 */

#include "transaction/tx_serialize.h"
#include "cbor.h"
#include <string.h>
#include <stdint.h>

/* Toutes les clés CBOR (signable et enveloppe complète) sont définies
 * dans tx_types.h afin de garder un mapping centralisé. */

/**
 * @brief Encode les champs "signables" dans un encoder CBOR existant.
 *
 * Fonction interne partagée entre tx_serialize_signable et tx_serialize_full.
 * Encode : type, from, to, amount, parents, timestamp.
 *
 * @param encoder Encoder CBOR déjà initialisé avec un container map ouvert
 * @param tx      Transaction source
 * @return CborNoError en cas de succès
 */
static CborError encode_signable_fields(CborEncoder *map_encoder, const transaction_t *tx)
{
    CborError err;

    /* type (clé 1) */
    err = cbor_encode_int(map_encoder, CBOR_KEY_TYPE);
    if (err != CborNoError) return err;
    err = cbor_encode_uint(map_encoder, (uint64_t)tx->type);
    if (err != CborNoError) return err;

    /* from (clé 2) — pour MINT, on encode quand même (32 octets nuls) */
    err = cbor_encode_int(map_encoder, CBOR_KEY_FROM);
    if (err != CborNoError) return err;
    err = cbor_encode_byte_string(map_encoder, tx->from.bytes, CRYPTO_PUBLIC_KEY_SIZE);
    if (err != CborNoError) return err;

    /* to (clé 3) */
    err = cbor_encode_int(map_encoder, CBOR_KEY_TO);
    if (err != CborNoError) return err;
    err = cbor_encode_byte_string(map_encoder, tx->to.bytes, CRYPTO_PUBLIC_KEY_SIZE);
    if (err != CborNoError) return err;

    /* amount (clé 4) */
    err = cbor_encode_int(map_encoder, CBOR_KEY_AMOUNT);
    if (err != CborNoError) return err;
    err = cbor_encode_uint(map_encoder, (uint64_t)tx->amount);
    if (err != CborNoError) return err;

    /* parents (clé 5) — tableau de byte strings */
    err = cbor_encode_int(map_encoder, CBOR_KEY_PARENTS);
    if (err != CborNoError) return err;

    CborEncoder array_encoder;
    err = cbor_encoder_create_array(map_encoder, &array_encoder, tx->parent_count);
    if (err != CborNoError) return err;

    for (uint8_t i = 0; i < tx->parent_count; i++) {
        err = cbor_encode_byte_string(&array_encoder, tx->parents[i].bytes, CRYPTO_HASH_SIZE);
        if (err != CborNoError) return err;
    }

    err = cbor_encoder_close_container(map_encoder, &array_encoder);
    if (err != CborNoError) return err;

    /* timestamp (clé 6) */
    err = cbor_encode_int(map_encoder, CBOR_KEY_TIMESTAMP);
    if (err != CborNoError) return err;
    err = cbor_encode_uint(map_encoder, tx->timestamp);
    if (err != CborNoError) return err;

    /* currency_id (clé 10) */
    err = cbor_encode_int(map_encoder, CBOR_KEY_CURRENCY);
    if (err != CborNoError) return err;
    err = cbor_encode_uint(map_encoder, (uint64_t)tx->currency_id);
    if (err != CborNoError) return err;

    /* fee (clé 11) — frais de transfert brûlés, inclus dans les champs
     * signables pour empêcher un attaquant de modifier le fee après coup */
    err = cbor_encode_int(map_encoder, CBOR_KEY_FEE);
    if (err != CborNoError) return err;
    err = cbor_encode_uint(map_encoder, (uint64_t)tx->fee);
    if (err != CborNoError) return err;

    /* seq (clé 12) — [I3-fix] Nonce monotone par emetteur. Inclus dans
     * les champs signables pour empecher un attaquant de changer le seq
     * et de creer artificiellement un conflit. */
    err = cbor_encode_int(map_encoder, CBOR_KEY_SEQ);
    if (err != CborNoError) return err;
    err = cbor_encode_uint(map_encoder, (uint64_t)tx->seq);
    if (err != CborNoError) return err;

    return CborNoError;
}

esp_err_t tx_serialize_signable(const transaction_t *tx,
                                uint8_t *buffer, size_t buffer_len,
                                size_t *out_len)
{
    if (tx == NULL || buffer == NULL || out_len == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    CborEncoder encoder;
    cbor_encoder_init(&encoder, buffer, buffer_len, 0);

    /* Map de 9 entrées : type, from, to, amount, parents, timestamp, currency_id, fee, seq */
    CborEncoder map_encoder;
    CborError err = cbor_encoder_create_map(&encoder, &map_encoder, 9);
    if (err != CborNoError) return ESP_ERR_NO_MEM;

    err = encode_signable_fields(&map_encoder, tx);
    if (err != CborNoError) return ESP_ERR_NO_MEM;

    err = cbor_encoder_close_container(&encoder, &map_encoder);
    if (err != CborNoError) return ESP_ERR_NO_MEM;

    *out_len = cbor_encoder_get_buffer_size(&encoder, buffer);

    /*
     * [F-TX-002] Garde de débordement par symétrie avec tx_serialize_full.
     * Tinycbor en mode "computation only" peut écrire au-delà du buffer
     * avant que close_container retourne CborErrorOutOfMemory. Sans ce
     * check, un payload signable > buffer_len corromprait la stack
     * (le buffer est typiquement TX_CBOR_MAX_SIZE = 320 sur la stack
     * dans `finalize_transaction`).
     */
    if (*out_len > buffer_len) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t tx_serialize_full(const transaction_t *tx,
                            uint8_t *buffer, size_t buffer_len,
                            size_t *out_len)
{
    if (tx == NULL || buffer == NULL || out_len == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    CborEncoder encoder;
    cbor_encoder_init(&encoder, buffer, buffer_len, 0);

    /* Map de 12 entrées : 9 signables (dont seq) + id + signature + status */
    CborEncoder map_encoder;
    CborError err = cbor_encoder_create_map(&encoder, &map_encoder, 12);
    if (err != CborNoError) return ESP_ERR_NO_MEM;

    /* Champs signables (type, from, to, amount, parents, timestamp) */
    err = encode_signable_fields(&map_encoder, tx);
    if (err != CborNoError) return ESP_ERR_NO_MEM;

    /* id (clé 7) */
    err = cbor_encode_int(&map_encoder, CBOR_KEY_ID);
    if (err != CborNoError) return ESP_ERR_NO_MEM;
    err = cbor_encode_byte_string(&map_encoder, tx->id.bytes, CRYPTO_HASH_SIZE);
    if (err != CborNoError) return ESP_ERR_NO_MEM;

    /* signature (clé 8) */
    err = cbor_encode_int(&map_encoder, CBOR_KEY_SIGNATURE);
    if (err != CborNoError) return ESP_ERR_NO_MEM;
    err = cbor_encode_byte_string(&map_encoder, tx->signature.bytes, CRYPTO_SIGNATURE_SIZE);
    if (err != CborNoError) return ESP_ERR_NO_MEM;

    /* status (clé 9) */
    err = cbor_encode_int(&map_encoder, CBOR_KEY_STATUS);
    if (err != CborNoError) return ESP_ERR_NO_MEM;
    err = cbor_encode_uint(&map_encoder, (uint64_t)tx->status);
    if (err != CborNoError) return ESP_ERR_NO_MEM;

    err = cbor_encoder_close_container(&encoder, &map_encoder);
    if (err != CborNoError) return ESP_ERR_NO_MEM;

    *out_len = cbor_encoder_get_buffer_size(&encoder, buffer);

    /* Vérification de la contrainte ESP-NOW */
    if (*out_len > TX_CBOR_MAX_SIZE) {
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

esp_err_t tx_deserialize(const uint8_t *buffer, size_t buffer_len,
                         transaction_t *tx)
{
    if (buffer == NULL || tx == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(tx, 0, sizeof(transaction_t));

    CborParser parser;
    CborValue root;
    CborError err = cbor_parser_init(buffer, buffer_len, 0, &parser, &root);
    if (err != CborNoError) return ESP_ERR_INVALID_ARG;

    /* Vérifier que la racine est une map */
    if (!cbor_value_is_map(&root)) {
        return ESP_ERR_INVALID_ARG;
    }

    CborValue map_iter;
    err = cbor_value_enter_container(&root, &map_iter);
    if (err != CborNoError) return ESP_ERR_INVALID_ARG;

    /*
     * [F-TX-001] Helper inline pour lire un uint64 CBOR avec garde de
     * type. Sans cette garde, en release (NDEBUG), `cbor_value_get_uint64`
     * lit un entier arbitraire sur un champ malformé (bstring au lieu
     * de uint), permettant à un attaquant LoRa d'injecter des valeurs
     * arbitraires dans amount/fee/seq/etc.
     */
    #define READ_UINT64_OR_FAIL(iter, out_val) do { \
        if (!cbor_value_is_unsigned_integer(iter)) { \
            return ESP_ERR_INVALID_ARG; \
        } \
        cbor_value_get_uint64((iter), (out_val)); \
    } while (0)

    /* Parcourir toutes les paires clé/valeur de la map */
    while (!cbor_value_at_end(&map_iter)) {
        /* Lire la clé (entier) */
        int key;
        if (!cbor_value_is_integer(&map_iter)) {
            return ESP_ERR_INVALID_ARG;
        }
        /*
         * [F-TX-010] `_checked` propage CborErrorDataTooLarge si la clé
         * dépasse INT_MAX (au lieu de tronquer silencieusement).
         */
        err = cbor_value_get_int_checked(&map_iter, &key);
        if (err != CborNoError) return ESP_ERR_INVALID_ARG;
        err = cbor_value_advance_fixed(&map_iter);
        if (err != CborNoError) return ESP_ERR_INVALID_ARG;

        /* Lire la valeur selon la clé */
        switch (key) {
        case CBOR_KEY_TYPE: {
            uint64_t val;
            READ_UINT64_OR_FAIL(&map_iter, &val);
            tx->type = (tx_type_t)val;
            err = cbor_value_advance_fixed(&map_iter);
            break;
        }
        case CBOR_KEY_FROM: {
            size_t len = CRYPTO_PUBLIC_KEY_SIZE;
            err = cbor_value_copy_byte_string(&map_iter, tx->from.bytes, &len, &map_iter);
            break;
        }
        case CBOR_KEY_TO: {
            size_t len = CRYPTO_PUBLIC_KEY_SIZE;
            err = cbor_value_copy_byte_string(&map_iter, tx->to.bytes, &len, &map_iter);
            break;
        }
        case CBOR_KEY_AMOUNT: {
            uint64_t val;
            READ_UINT64_OR_FAIL(&map_iter, &val);
            /* Protection contre la troncature uint64→uint32 : un montant
             * dépassant UINT32_MAX serait silencieusement tronqué, ce qui
             * pourrait être exploité par un attaquant. */
            if (val > UINT32_MAX) {
                return ESP_ERR_INVALID_ARG;
            }
            tx->amount = (uint32_t)val;
            err = cbor_value_advance_fixed(&map_iter);
            break;
        }
        case CBOR_KEY_PARENTS: {
            /* Tableau de byte strings */
            CborValue array_iter;
            err = cbor_value_enter_container(&map_iter, &array_iter);
            if (err != CborNoError) break;

            tx->parent_count = 0;
            while (!cbor_value_at_end(&array_iter) && tx->parent_count < TX_MAX_PARENTS) {
                size_t len = CRYPTO_HASH_SIZE;
                err = cbor_value_copy_byte_string(&array_iter,
                    tx->parents[tx->parent_count].bytes, &len, &array_iter);
                if (err != CborNoError) break;
                tx->parent_count++;
            }
            /*
             * [F-TX-007] Si le tableau CBOR contenait plus que TX_MAX_PARENTS
             * éléments, on ne les a pas tous lus — on rejette la TX au lieu
             * de la tronquer silencieusement. Sans ce check, un CBOR forgé
             * avec N+1 parents passerait avec seulement N parents lus, créant
             * une ambiguïté entre le CBOR reçu et la TX traitée.
             */
            if (err == CborNoError && !cbor_value_at_end(&array_iter)) {
                return ESP_ERR_INVALID_ARG;
            }
            err = cbor_value_leave_container(&map_iter, &array_iter);
            break;
        }
        case CBOR_KEY_TIMESTAMP: {
            uint64_t val;
            READ_UINT64_OR_FAIL(&map_iter, &val);
            tx->timestamp = val;
            err = cbor_value_advance_fixed(&map_iter);
            break;
        }
        case CBOR_KEY_ID: {
            size_t len = CRYPTO_HASH_SIZE;
            err = cbor_value_copy_byte_string(&map_iter, tx->id.bytes, &len, &map_iter);
            break;
        }
        case CBOR_KEY_SIGNATURE: {
            size_t len = CRYPTO_SIGNATURE_SIZE;
            err = cbor_value_copy_byte_string(&map_iter, tx->signature.bytes, &len, &map_iter);
            break;
        }
        case CBOR_KEY_STATUS: {
            /*
             * SÉCURITÉ : on ignore le champ status reçu du réseau.
             *
             * Le status (LOCKED/CONFIRMED/CANCELLED) n'est PAS couvert par
             * le hash ni la signature de la transaction. Un attaquant pourrait
             * donc modifier le CBOR pour passer une TX de LOCKED à CONFIRMED
             * sans invalider la signature.
             *
             * Politique :
             * - TRANSFER reçus du réseau → forcé à LOCKED (le destinataire
             *   devra confirmer localement via le protocole ACK).
             * - MINT → accepté comme CONFIRMED (la création de monnaie est
             *   confirmée dès la signature par le maître).
             *
             * On lit quand même la valeur CBOR pour avancer l'itérateur,
             * mais on ne l'utilise pas.
             */
            uint64_t val;
            READ_UINT64_OR_FAIL(&map_iter, &val);
            (void)val;  /* Valeur réseau ignorée intentionnellement */
            err = cbor_value_advance_fixed(&map_iter);
            break;
        }
        case CBOR_KEY_CURRENCY: {
            uint64_t val;
            READ_UINT64_OR_FAIL(&map_iter, &val);
            /* Protection contre la troncature uint64→uint32 : un currency_id
             * dépassant UINT32_MAX serait silencieusement tronqué. */
            if (val > UINT32_MAX) {
                return ESP_ERR_INVALID_ARG;
            }
            tx->currency_id = (uint32_t)val;
            err = cbor_value_advance_fixed(&map_iter);
            break;
        }
        case CBOR_KEY_FEE: {
            /*
             * Frais de transfert stockés dans la transaction au moment de
             * sa création. Si absent (anciennes TX sans fee), la valeur
             * reste à 0 grâce au memset initial (rétrocompatibilité).
             */
            uint64_t val;
            READ_UINT64_OR_FAIL(&map_iter, &val);
            /* Protection contre la troncature uint64→uint32 */
            if (val > UINT32_MAX) {
                return ESP_ERR_INVALID_ARG;
            }
            tx->fee = (uint32_t)val;
            err = cbor_value_advance_fixed(&map_iter);
            break;
        }
        case CBOR_KEY_SEQ: {
            /*
             * [I3-fix] Nonce monotone par emetteur.
             * Si absent (anciennes TX sans seq), reste a 0 — les conflits
             * ne seront pas detectes pour ces TX, mais la compatibilite
             * ascendante est preservee.
             */
            uint64_t val;
            READ_UINT64_OR_FAIL(&map_iter, &val);
            if (val > UINT32_MAX) {
                return ESP_ERR_INVALID_ARG;
            }
            tx->seq = (uint32_t)val;
            err = cbor_value_advance_fixed(&map_iter);
            break;
        }
        default:
            /* Clé inconnue : on l'ignore (extensibilité CBOR) */
            err = cbor_value_advance(&map_iter);
            break;
        }

        if (err != CborNoError) return ESP_ERR_INVALID_ARG;
    }

    /*
     * Forcer le status de la transaction selon son type.
     * Voir le commentaire dans le case CBOR_KEY_STATUS pour la justification.
     */
    if (tx->type == TX_TYPE_MINT) {
        tx->status = TX_STATUS_CONFIRMED;
    } else {
        tx->status = TX_STATUS_LOCKED;
    }

    return ESP_OK;
}
