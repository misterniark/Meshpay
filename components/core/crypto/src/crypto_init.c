/**
 * @file crypto_init.c
 * @brief Initialisation centralisée et thread-safe du sous-systeme crypto.
 *
 * Historique :
 *   - C6 (audit Sonnet, avril 2026) : centralisation de l'init PSA Crypto
 *     pour eviter une race condition entre tasks (crypto_keys / crypto_sign
 *     appelaient `psa_crypto_init()` chacun via un flag local).
 *   - Lot E.2 (mai 2026) : suppression de la dependance PSA Crypto suite
 *     a la decouverte que mbedTLS d'IDF v5.4.3 n'inclut pas de driver
 *     Ed25519 (`PSA_ERROR_NOT_SUPPORTED`). Le composant utilise desormais
 *     Monocypher (vendore), qui ne requiert pas d'init globale.
 *
 * Ce qui reste : le flag `s_crypto_initialized` et son mutex de creation
 * statique. C'est volontaire :
 *   1. Garde l'invariant "crypto_is_initialized() doit etre vrai avant
 *      tout appel a crypto_generate_keypair / crypto_sign / crypto_verify"
 *      introduit par C6 (et verifie par chaque fonction).
 *   2. Permet a un futur remplacement crypto (HSM, secure element, retour
 *      a PSA si IDF v6 corrige le manque) de reactiver l'init reelle
 *      sans toucher aux callers.
 *   3. Coute zero : un bool + un mutex statique = ~10 octets BSS.
 */

#include "crypto/crypto_init.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

/** Flag global indiquant si le sous-systeme crypto est initialise */
static bool s_crypto_initialized = false;

/** Mutex protegeant l'initialisation contre les appels concurrents */
static SemaphoreHandle_t s_init_mutex = NULL;

/**
 * @brief Mutex statique utilise pour eviter une allocation dynamique
 * fragile au demarrage.
 */
static StaticSemaphore_t s_init_mutex_buffer;

esp_err_t crypto_init(void)
{
    /*
     * Creation du mutex au premier appel.
     * Mutex statique : pas d'echec memoire possible.
     */
    if (s_init_mutex == NULL) {
        s_init_mutex = xSemaphoreCreateMutexStatic(&s_init_mutex_buffer);
    }

    /* Acquisition du mutex avec timeout infini */
    if (xSemaphoreTake(s_init_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_FAIL;
    }

    /* Idempotent : double init = no-op */
    if (s_crypto_initialized) {
        xSemaphoreGive(s_init_mutex);
        return ESP_OK;
    }

    /*
     * Depuis Lot E.2, plus rien a initialiser cote crypto : Monocypher
     * n'a pas d'etat partage. On bascule juste le flag.
     *
     * Si un futur backend crypto necessite une init reelle (PSA driver
     * Ed25519, secure element, HSM), c'est ici qu'il faut la placer.
     */
    s_crypto_initialized = true;
    xSemaphoreGive(s_init_mutex);
    return ESP_OK;
}

bool crypto_is_initialized(void)
{
    return s_crypto_initialized;
}
