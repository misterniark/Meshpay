/**
 * @file crypto_init.h
 * @brief Initialisation centralisée du sous-système cryptographique PSA.
 *
 * Fournit un point d'entrée unique et thread-safe pour initialiser
 * PSA Crypto. Doit être appelé une fois au démarrage de l'application,
 * avant toute utilisation des fonctions de crypto_keys ou crypto_sign.
 */

#ifndef CRYPTO_INIT_H
#define CRYPTO_INIT_H

#include "esp_err.h"
#include <stdbool.h>

/**
 * @brief Initialise le sous-système PSA Crypto (thread-safe).
 *
 * Cette fonction est protégée par un mutex FreeRTOS pour éviter
 * les race conditions si elle est appelée depuis plusieurs tâches.
 * Elle est idempotente : les appels après le premier sont des no-ops.
 *
 * @return ESP_OK si l'initialisation a réussi ou était déjà faite,
 *         ESP_FAIL en cas d'erreur de PSA Crypto
 */
esp_err_t crypto_init(void);

/**
 * @brief Vérifie si le sous-système crypto est initialisé.
 *
 * Permet aux fonctions internes de vérifier que crypto_init()
 * a bien été appelé avant de procéder.
 *
 * @return true si initialisé, false sinon
 */
bool crypto_is_initialized(void);

#endif /* CRYPTO_INIT_H */
