/**
 * @file hal_storage_esp32.c
 * @brief Implémentation NVS du stockage persistant pour ESP32.
 *
 * Stratégie : ouvrir/fermer le handle NVS à chaque opération.
 * C'est plus simple que de cacher des handles, et NVS est protégé
 * par mutex en interne, donc thread-safe.
 *
 * Flux pour chaque opération d'écriture :
 * 1. nvs_open(namespace, NVS_READWRITE, &handle)
 * 2. nvs_set_xxx(handle, key, value)
 * 3. nvs_commit(handle)
 * 4. nvs_close(handle)
 * 5. Traduire esp_err_t → hal_err_t
 */

#include "hal_storage_esp32.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"

static const char *TAG = "hal_storage_esp32";

/* --- Contexte interne --- */

typedef struct {
    bool initialized; /* true après nvs_flash_init réussi */
} esp32_storage_ctx_t;

/* Contexte statique — un seul device, un seul storage */
static esp32_storage_ctx_t s_esp32_ctx;

/* --- Traduction des erreurs ESP-IDF vers HAL --- */

/**
 * Convertir un esp_err_t en hal_err_t.
 * Couvre les cas courants de NVS, les autres deviennent HAL_FAIL.
 */
static hal_err_t esp_to_hal(esp_err_t err)
{
    switch (err) {
        case ESP_OK:                  return HAL_OK;
        case ESP_ERR_NVS_NOT_FOUND:   return HAL_ERR_NOT_FOUND;
        case ESP_ERR_NVS_NO_FREE_PAGES:
        case ESP_ERR_NO_MEM:          return HAL_ERR_NO_MEM;
        case ESP_ERR_NVS_INVALID_NAME:
        case ESP_ERR_NVS_KEY_TOO_LONG:
        case ESP_ERR_INVALID_ARG:     return HAL_ERR_INVALID;
        default:
            ESP_LOGW(TAG, "Erreur NVS non mappée : 0x%x", err);
            return HAL_FAIL;
    }
}

/* --- Implémentation des opérations de la vtable --- */

static hal_err_t esp32_u32_write(const char *ns, const char *key,
                                 uint32_t value, void *ctx)
{
    (void)ctx;
    nvs_handle_t handle;
    esp_err_t err = nvs_open(ns, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return esp_to_hal(err);
    }

    err = nvs_set_u32(handle, key, value);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return esp_to_hal(err);
}

static hal_err_t esp32_u32_read(const char *ns, const char *key,
                                uint32_t *value, void *ctx)
{
    (void)ctx;
    nvs_handle_t handle;
    esp_err_t err = nvs_open(ns, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return esp_to_hal(err);
    }

    err = nvs_get_u32(handle, key, value);
    nvs_close(handle);
    return esp_to_hal(err);
}

static hal_err_t esp32_blob_write(const char *ns, const char *key,
                                  const uint8_t *data, size_t len, void *ctx)
{
    (void)ctx;
    nvs_handle_t handle;
    esp_err_t err = nvs_open(ns, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return esp_to_hal(err);
    }

    err = nvs_set_blob(handle, key, data, len);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return esp_to_hal(err);
}

/**
 * Lire un blob depuis NVS.
 *
 * Si buf est NULL, on utilise nvs_get_blob avec length=0 pour
 * récupérer uniquement la taille stockée.
 */
static hal_err_t esp32_blob_read(const char *ns, const char *key,
                                 uint8_t *buf, size_t *len, void *ctx)
{
    (void)ctx;
    nvs_handle_t handle;
    esp_err_t err = nvs_open(ns, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return esp_to_hal(err);
    }

    if (!buf) {
        /* Mode requête de taille : passer len=0 à NVS */
        *len = 0;
        err = nvs_get_blob(handle, key, NULL, len);
    } else {
        err = nvs_get_blob(handle, key, buf, len);
    }

    nvs_close(handle);

    /* NVS retourne ESP_ERR_NVS_INVALID_LENGTH si le buffer est trop petit */
    if (err == ESP_ERR_NVS_INVALID_LENGTH) {
        return HAL_ERR_NO_MEM;
    }
    return esp_to_hal(err);
}

static hal_err_t esp32_erase(const char *ns, const char *key, void *ctx)
{
    (void)ctx;
    nvs_handle_t handle;
    esp_err_t err = nvs_open(ns, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return esp_to_hal(err);
    }

    err = nvs_erase_key(handle, key);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return esp_to_hal(err);
}

static hal_err_t esp32_exists(const char *ns, const char *key,
                              bool *exists_out, void *ctx)
{
    (void)ctx;
    nvs_handle_t handle;
    esp_err_t err = nvs_open(ns, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        /* Si le namespace n'existe pas encore, la clé n'existe pas */
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            *exists_out = false;
            return HAL_OK;
        }
        return esp_to_hal(err);
    }

    /*
     * Astuce : tenter de lire la taille d'un blob avec un buffer NULL.
     * Si la clé existe (quel que soit le type), NVS retourne OK ou
     * une erreur de type. On tente d'abord u32 puis blob.
     */
    uint32_t dummy_u32;
    err = nvs_get_u32(handle, key, &dummy_u32);
    if (err == ESP_OK) {
        *exists_out = true;
        nvs_close(handle);
        return HAL_OK;
    }

    /* Tenter comme blob */
    size_t dummy_len = 0;
    err = nvs_get_blob(handle, key, NULL, &dummy_len);
    *exists_out = (err == ESP_OK);
    nvs_close(handle);
    return HAL_OK;
}

/* --- Factory --- */

hal_err_t hal_storage_esp32_create(hal_storage_t *storage)
{
    if (!storage) {
        return HAL_ERR_INVALID;
    }

    /*
     * Vérifier que NVS est déjà initialisé.
     *
     * L'initialisation NVS (standard ou chiffrée) est faite dans app_main()
     * AVANT la création du HAL storage. Ce bloc est un filet de sécurité
     * au cas où le HAL serait utilisé indépendamment.
     *
     * Note : quand CONFIG_NVS_ENCRYPTION est actif, nvs_flash_init() seul
     * ne suffit pas — il faut nvs_flash_secure_init() avec les clés.
     * Mais si NVS est déjà initialisé, nvs_flash_init() retourne ESP_OK
     * sans rien faire, ce qui est correct.
     */
    if (!s_esp32_ctx.initialized) {
        esp_err_t err = nvs_flash_init();

        if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
            err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
            ESP_LOGW(TAG, "NVS corrompu, effacement et réinit...");
            ESP_ERROR_CHECK(nvs_flash_erase());
            err = nvs_flash_init();
        }

        if (err != ESP_OK) {
            ESP_LOGE(TAG, "nvs_flash_init échoué : 0x%x", err);
            return HAL_FAIL;
        }

        s_esp32_ctx.initialized = true;
        ESP_LOGI(TAG, "NVS initialisé avec succès");
    }

    /* Remplir la vtable */
    storage->u32_write  = esp32_u32_write;
    storage->u32_read   = esp32_u32_read;
    storage->blob_write = esp32_blob_write;
    storage->blob_read  = esp32_blob_read;
    storage->erase      = esp32_erase;
    storage->exists     = esp32_exists;
    storage->ctx        = &s_esp32_ctx;

    return HAL_OK;
}
