/**
 * @file ops/ops.h
 * @brief Operations initiees localement (depuis l'UI ou app_main).
 *
 * Toutes ces operations sont DEJA appelees sous s_state_mutex
 * par leur invocant (handle_ui_command ou core_task). Voir [C1-fix].
 *
 * Implementations dans `op_*.c` (Lot D.5).
 */

#ifndef MESHPAY_OPS_H
#define MESHPAY_OPS_H

#include <stdint.h>
#include "esp_err.h"
#include "crypto/crypto_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initie un paiement TRANSFER vers `to` d'un montant `amount`.
 * Verifie solde + fonte, prend des tips, cree+signe la TX, lock_table,
 * envoi ESP-NOW. Sync LoRa en differe via lora_sync_task.
 */
esp_err_t initiate_payment(const public_key_t *to, uint32_t amount);

/**
 * @brief Cree une TX MINT (maitre uniquement). Verifie que device est
 * dans mint_authorities.
 */
esp_err_t initiate_mint(const public_key_t *to, uint32_t amount);

/**
 * @brief Tente un transfert automatique du solde vers le beneficiaire
 * configure. Appele periodiquement par core_task quand `s_forward_interval_min > 0`.
 */
void attempt_beneficiary_forward(void);

/* ----------------------------------------------------------------
 * Operations maitre (signe + envoi LoRa). Compilent partout, runtime
 * check `is_master` retourne ESP_ERR_NOT_ALLOWED sur non-maitre.
 *
 * Le LoRa est obligatoire sur toutes les cibles : transport_lora_send
 * est toujours l'impl reelle ; un non-maitre est filtre par le runtime
 * check `is_master`.
 * ---------------------------------------------------------------- */

esp_err_t broadcast_text_send(const char *text, uint8_t text_len);
esp_err_t ping_send(void);
esp_err_t set_alias_send(const public_key_t *target_key,
                         const char *alias, uint8_t alias_len);
esp_err_t set_beneficiary_send(const public_key_t *target_key,
                               const public_key_t *beneficiary_key,
                               uint16_t forward_interval_min);

#ifdef __cplusplus
}
#endif

#endif /* MESHPAY_OPS_H */
