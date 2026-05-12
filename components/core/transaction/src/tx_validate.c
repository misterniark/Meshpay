/**
 * @file tx_validate.c
 * @brief Implémentation de la validation des transactions.
 *
 * Trois niveaux de validation :
 * 1. Structure : vérification des invariants (champs obligatoires, bornes)
 * 2. Signature : recalcul du hash et vérification de la signature Ed25519
 * 3. Autorisation maître : vérification que l'émetteur d'un MINT est autorisé
 */

#include "transaction/tx_validate.h"
#include "transaction/tx_serialize.h"
#include "crypto/crypto_hash.h"
#include "crypto/crypto_sign.h"
#include <string.h>

/**
 * @brief Vérifie si une clé publique est entièrement nulle.
 *
 * Utilisé pour vérifier que les champs "from" et "to" contiennent
 * bien une clé publique valide (non-nulle).
 *
 * @param key Clé publique à vérifier
 * @return true si tous les octets sont à zéro
 */
static bool is_key_zero(const public_key_t *key)
{
    for (int i = 0; i < CRYPTO_PUBLIC_KEY_SIZE; i++) {
        if (key->bytes[i] != 0) return false;
    }
    return true;
}

esp_err_t tx_validate_structure(const transaction_t *tx)
{
    if (tx == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Le montant doit être strictement positif */
    if (tx->amount == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Le nombre de parents doit être entre 1 et TX_MAX_PARENTS */
    if (tx->parent_count == 0 || tx->parent_count > TX_MAX_PARENTS) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Au moins un parent doit être non-nul */
    bool has_valid_parent = false;
    for (uint8_t i = 0; i < tx->parent_count; i++) {
        if (!hash_is_zero(&tx->parents[i])) {
            has_valid_parent = true;
            break;
        }
    }
    if (!has_valid_parent) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Le type doit être valide */
    if (tx->type != TX_TYPE_TRANSFER && tx->type != TX_TYPE_MINT) {
        return ESP_ERR_INVALID_ARG;
    }

    /*
     * Vérifications spécifiques au type.
     *
     * MINT : le champ "from" contient la clé publique du device maître
     * qui a signé la création de monnaie. Il doit être non-nul pour
     * permettre la vérification de signature (voir tx_validate_signature).
     *
     * TRANSFER : le champ "from" est l'émetteur, il doit être non-nul.
     */
    if (is_key_zero(&tx->from)) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Le destinataire doit être non-nul */
    if (is_key_zero(&tx->to)) {
        return ESP_ERR_INVALID_ARG;
    }

    return ESP_OK;
}

esp_err_t tx_validate_signature(const transaction_t *tx)
{
    if (tx == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /*
     * Recalculer le contenu signable en CBOR, puis vérifier :
     * 1. Le hash SHA-256 correspond au champ id
     * 2. La signature Ed25519 est valide
     */
    uint8_t cbor_buf[200];
    size_t cbor_len = 0;

    esp_err_t err = tx_serialize_signable(tx, cbor_buf, sizeof(cbor_buf), &cbor_len);
    if (err != ESP_OK) {
        return ESP_ERR_INVALID_STATE;
    }

    /* Vérifier le hash (id) */
    hash_t computed_hash;
    err = crypto_hash_sha256(cbor_buf, cbor_len, &computed_hash);
    if (err != ESP_OK) {
        return ESP_ERR_INVALID_STATE;
    }

    if (!hash_equal(&computed_hash, &tx->id)) {
        return ESP_ERR_INVALID_STATE;
    }

    /*
     * Vérifier la signature Ed25519.
     * Pour un TRANSFER : vérifier avec la clé publique "from" (l'émetteur).
     * Pour un MINT : vérifier également avec "from" — la clé du maître
     * signataire. Le champ "from" d'un MINT contient la clé publique du
     * device maître qui a créé la monnaie (corrigé : on ne skip plus les
     * MINT, car un attaquant pourrait forger un MINT avec une signature
     * invalide et l'injecter dans le DAG).
     *
     * Note : tx_validate_master() vérifie en plus que la clé "from" fait
     * partie de la liste des maîtres autorisés (vérification d'autorité).
     */
    err = crypto_verify(cbor_buf, cbor_len, &tx->from, &tx->signature);
    if (err != ESP_OK) {
        return ESP_ERR_INVALID_STATE;
    }

    return ESP_OK;
}

esp_err_t tx_validate_master(const transaction_t *tx, const master_keys_t *master_keys)
{
    if (tx == NULL || master_keys == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Les TRANSFER n'ont pas besoin de vérification maître */
    if (tx->type == TX_TYPE_TRANSFER) {
        return ESP_OK;
    }

    /*
     * Pour un MINT, le champ "from" contient la clé publique du maître
     * signataire. On vérifie que cette clé fait partie de la liste
     * des maîtres autorisés.
     *
     * La vérification de la signature elle-même est déjà faite par
     * tx_validate_signature() (qui utilise tx->from comme clé de
     * vérification, y compris pour les MINT).
     */
    for (uint8_t i = 0; i < master_keys->count; i++) {
        if (memcmp(tx->from.bytes, master_keys->keys[i].bytes,
                   CRYPTO_PUBLIC_KEY_SIZE) == 0) {
            return ESP_OK;  /* Clé maître autorisée */
        }
    }

    /* La clé "from" n'est pas dans la liste des maîtres autorisés */
    return ESP_ERR_INVALID_STATE;
}
