/**
 * @file tx_create.h
 * @brief Création de transactions (TRANSFER et MINT).
 *
 * Fournit les fonctions pour créer des transactions complètes :
 * - Calcul du hash (id) à partir du contenu sérialisé
 * - Signature automatique avec la clé privée de l'émetteur
 * - Validation des paramètres d'entrée
 *
 * Le workflow de création est :
 * 1. Remplir les champs de la transaction (type, from, to, amount, parents, timestamp)
 * 2. Sérialiser le contenu "signable" (sans id ni signature) en CBOR
 * 3. Calculer le hash SHA-256 du CBOR → c'est l'id
 * 4. Signer le CBOR avec la clé privée → c'est la signature
 */

#ifndef TX_CREATE_H
#define TX_CREATE_H

#include "transaction/tx_types.h"
#include "crypto/crypto_types.h"
#include "esp_err.h"

/**
 * @brief Crée une transaction de type TRANSFER.
 *
 * Le montant est transféré de l'émetteur (keypair) vers le destinataire (to).
 * La transaction est automatiquement signée et son id (hash) calculé.
 * Le statut initial est LOCKED (en attente de confirmation).
 *
 * @param[out] tx           Transaction créée
 * @param[in]  keypair      Clé privée + publique de l'émetteur
 * @param[in]  to           Clé publique du destinataire
 * @param[in]  amount       Montant en crédits (> 0)
 * @param[in]  currency_id  Identifiant de la monnaie (inclus dans le contenu signé)
 * @param[in]  fee          Frais de transfert brûlés (inclus dans le contenu signé)
 * @param[in]  parents      Tableau de hashes des transactions parentes
 * @param[in]  parent_count Nombre de parents (1 ou 2)
 * @param[in]  timestamp    Horodatage en millisecondes
 * @return ESP_OK en cas de succès
 *         ESP_ERR_INVALID_ARG si paramètres invalides (amount=0, parent_count>2, etc.)
 */
esp_err_t tx_create_transfer(transaction_t *tx,
                             const keypair_t *keypair,
                             const public_key_t *to,
                             uint32_t amount,
                             uint32_t currency_id,
                             uint32_t fee,
                             uint32_t seq,
                             const hash_t *parents,
                             uint8_t parent_count,
                             uint64_t timestamp);

/**
 * @brief Crée une transaction de type MINT (création de crédits).
 *
 * Seul un device maître peut créer des crédits. La transaction n'a pas
 * de champ "from" (l'émetteur est implicitement le device maître).
 * Le statut initial est CONFIRMED (pas besoin d'ACK pour un MINT).
 *
 * @param[out] tx           Transaction créée
 * @param[in]  master_keypair Clé privée + publique du device maître
 * @param[in]  to           Clé publique du destinataire des crédits
 * @param[in]  amount       Montant de crédits à créer (> 0)
 * @param[in]  currency_id  Identifiant de la monnaie (inclus dans le contenu signé)
 * @param[in]  parents      Tableau de hashes des transactions parentes
 * @param[in]  parent_count Nombre de parents (1 ou 2)
 * @param[in]  timestamp    Horodatage en millisecondes
 * @return ESP_OK en cas de succès
 *         ESP_ERR_INVALID_ARG si paramètres invalides
 */
esp_err_t tx_create_mint(transaction_t *tx,
                         const keypair_t *master_keypair,
                         const public_key_t *to,
                         uint32_t amount,
                         uint32_t currency_id,
                         uint32_t seq,
                         const hash_t *parents,
                         uint8_t parent_count,
                         uint64_t timestamp);

#endif /* TX_CREATE_H */
