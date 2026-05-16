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

/*
 * [F-TX-011] Helper local `is_key_zero` supprimé : on utilise désormais
 * la fonction publique `public_key_is_zero` exposée par crypto_types.h.
 * Cela élimine la duplication et le risque de divergence silencieuse.
 */

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

    /*
     * [F-TX-009] TOUS les parents déclarés (index 0..parent_count-1)
     * doivent être non-nuls. Une TX qui annonce `parent_count = 2`
     * avec `parents[1] = {0}` est rejetée comme incohérente : si un
     * slot doit être vide, il ne doit pas être compté dans
     * parent_count. Renforce la topologie du DAG.
     */
    for (uint8_t i = 0; i < tx->parent_count; i++) {
        if (hash_is_zero(&tx->parents[i])) {
            return ESP_ERR_INVALID_ARG;
        }
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
    if (public_key_is_zero(&tx->from)) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Le destinataire doit être non-nul */
    if (public_key_is_zero(&tx->to)) {
        return ESP_ERR_INVALID_ARG;
    }

    /*
     * [F-DG-020] TX TRANSFER : interdire from == to (self-loop).
     *
     * Décision produit : un device ne doit pas pouvoir s'envoyer un
     * paiement à lui-même. Sans cette garde, l'utilisateur pouvait
     * configurer accidentellement son propre alias comme bénéficiaire
     * dans op_beneficiary_forward, ou saisir sa propre clé comme
     * destinataire dans l'écran Payer — résultat : perte nette de
     * `fee` à chaque TX confirmée, sans gain comptable réel.
     *
     * Pour les MINT, from == to est autorisé : le maître peut
     * légitimement créer de la monnaie à son propre nom (genesis MINT
     * d'auto-attribution). Donc la garde s'applique uniquement aux
     * TRANSFER.
     *
     * Conséquence côté UI : l'écran "Payer" doit déjà filtrer la
     * pubkey du device propriétaire dans la liste des destinataires,
     * et l'écran de configuration bénéficiaire doit interdire la
     * propre clé. Ce check est le dernier filet de sécurité.
     */
    if (tx->type == TX_TYPE_TRANSFER &&
        memcmp(tx->from.bytes, tx->to.bytes, CRYPTO_PUBLIC_KEY_SIZE) == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    /*
     * [F-TX-003] TRANSFER : fee strictement inférieur à amount.
     * Décision design 2026-05-16. La règle `fee < amount` garantit que
     * le destinataire reçoit toujours un montant net strictement
     * positif. Un fee == amount produirait un transfert net nul
     * (destinataire reçoit 0), considéré comme dégénéré.
     * Les MINT n'ont pas de fee — pas de check.
     */
    if (tx->type == TX_TYPE_TRANSFER && tx->fee >= tx->amount) {
        return ESP_ERR_INVALID_ARG;
    }

    /*
     * [F-TX-005] TRANSFER : seq doit être strictement > 0.
     * Le seq=0 est réservé au genesis MINT de l'émetteur. Une TX
     * TRANSFER avec seq=0 indique un wallet qui a perdu son compteur
     * NVS et n'a pas pu le récupérer via F-DG-011 — c'est un état
     * pathologique qui produirait des conflits silencieux côté merge.
     * Mieux vaut rejeter la TX à la source.
     */
    if (tx->type == TX_TYPE_TRANSFER && tx->seq == 0) {
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
    uint8_t cbor_buf[TX_CBOR_MAX_SIZE];
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
