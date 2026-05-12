/**
 * @file tx_create.c
 * @brief Implémentation de la création de transactions.
 *
 * Workflow de création :
 * 1. Remplir les champs de la transaction
 * 2. Sérialiser le contenu "signable" en CBOR (type, from, to, amount, parents, timestamp)
 * 3. Hasher le CBOR → id de la transaction
 * 4. Signer le CBOR → signature Ed25519
 *
 * Le même buffer CBOR sert pour le hash ET la signature, garantissant
 * la cohérence entre les deux.
 */

#include "transaction/tx_create.h"
#include "transaction/tx_serialize.h"
#include "crypto/crypto_hash.h"
#include "crypto/crypto_sign.h"
#include <string.h>

/**
 * @brief Buffer temporaire pour la sérialisation CBOR lors de la création.
 *
 * Utilisé pour sérialiser le contenu signable avant de le hasher et signer.
 * La taille est suffisante pour tous les champs signables.
 */
#define TX_CREATE_BUFFER_SIZE 200

/**
 * @brief Remplit les champs communs et calcule le hash + signature.
 *
 * Fonction interne appelée par tx_create_transfer et tx_create_mint
 * après remplissage des champs spécifiques au type.
 *
 * @param tx      Transaction avec les champs déjà remplis (sauf id et signature)
 * @param keypair Clé privée pour la signature
 * @return ESP_OK en cas de succès
 */
static esp_err_t finalize_transaction(transaction_t *tx, const keypair_t *keypair)
{
    uint8_t cbor_buf[TX_CREATE_BUFFER_SIZE];
    size_t cbor_len = 0;

    /* Sérialiser le contenu signable en CBOR */
    esp_err_t err = tx_serialize_signable(tx, cbor_buf, sizeof(cbor_buf), &cbor_len);
    if (err != ESP_OK) {
        return err;
    }

    /* Calculer le hash SHA-256 du CBOR → id de la transaction */
    err = crypto_hash_sha256(cbor_buf, cbor_len, &tx->id);
    if (err != ESP_OK) {
        return err;
    }

    /* Signer le même buffer CBOR → signature de la transaction */
    err = crypto_sign(cbor_buf, cbor_len, keypair, &tx->signature);
    if (err != ESP_OK) {
        return err;
    }

    return ESP_OK;
}

esp_err_t tx_create_transfer(transaction_t *tx,
                             const keypair_t *keypair,
                             const public_key_t *to,
                             uint32_t amount,
                             uint32_t currency_id,
                             uint32_t fee,
                             uint32_t seq,
                             const hash_t *parents,
                             uint8_t parent_count,
                             uint64_t timestamp)
{
    /* Validation des paramètres */
    if (tx == NULL || keypair == NULL || to == NULL || parents == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (amount == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (parent_count == 0 || parent_count > TX_MAX_PARENTS) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Initialiser la structure à zéro */
    memset(tx, 0, sizeof(transaction_t));

    /* Remplir les champs */
    tx->type = TX_TYPE_TRANSFER;
    memcpy(&tx->from, &keypair->public_key, sizeof(public_key_t));
    memcpy(&tx->to, to, sizeof(public_key_t));
    tx->amount = amount;
    tx->currency_id = currency_id;
    tx->fee = fee;  /* Frais de transfert figés au moment de la création */
    tx->seq = seq;  /* [I3-fix] Nonce monotone de l'emetteur */
    tx->parent_count = parent_count;
    for (uint8_t i = 0; i < parent_count; i++) {
        memcpy(&tx->parents[i], &parents[i], sizeof(hash_t));
    }
    tx->timestamp = timestamp;
    tx->status = TX_STATUS_LOCKED;  /* TRANSFER commence toujours en LOCKED */

    /* Calculer hash et signature */
    return finalize_transaction(tx, keypair);
}

esp_err_t tx_create_mint(transaction_t *tx,
                         const keypair_t *master_keypair,
                         const public_key_t *to,
                         uint32_t amount,
                         uint32_t currency_id,
                         uint32_t seq,
                         const hash_t *parents,
                         uint8_t parent_count,
                         uint64_t timestamp)
{
    /* Validation des paramètres */
    if (tx == NULL || master_keypair == NULL || to == NULL || parents == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (amount == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (parent_count == 0 || parent_count > TX_MAX_PARENTS) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Initialiser la structure à zéro */
    memset(tx, 0, sizeof(transaction_t));

    /*
     * Remplir les champs.
     * Pour un MINT, le champ "from" contient la clé publique du device
     * maître qui signe la création de monnaie. Cela permet de vérifier
     * la signature Ed25519 sans connaître la liste des maîtres, et
     * d'identifier quel maître a émis le MINT.
     */
    tx->type = TX_TYPE_MINT;
    memcpy(&tx->from, &master_keypair->public_key, sizeof(public_key_t));
    memcpy(&tx->to, to, sizeof(public_key_t));
    tx->amount = amount;
    tx->currency_id = currency_id;
    tx->fee = 0;  /* Les MINT n'ont pas de frais de transfert */
    tx->seq = seq;  /* [I3-fix] Nonce monotone meme pour MINT */
    tx->parent_count = parent_count;
    for (uint8_t i = 0; i < parent_count; i++) {
        memcpy(&tx->parents[i], &parents[i], sizeof(hash_t));
    }
    tx->timestamp = timestamp;
    tx->status = TX_STATUS_CONFIRMED;  /* MINT est immédiatement confirmé */

    /* Calculer hash et signature (avec la clé maître) */
    return finalize_transaction(tx, master_keypair);
}
