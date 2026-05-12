/**
 * @file hal_storage_mock.c
 * @brief Implémentation mock du stockage — dictionnaire en mémoire RAM.
 *
 * Utilise un tableau statique de MOCK_MAX_ENTRIES entrées.
 * Chaque entrée stocke : namespace, clé, données brutes, taille, et flag utilisé.
 * La recherche est linéaire (O(n)) — largement suffisant pour les tests.
 */

#include "hal_storage_mock.h"
#include <string.h>

/* --- Constantes internes --- */

/** Nombre maximum d'entrées dans le mock */
#define MOCK_MAX_ENTRIES   64

/** Taille maximum d'un namespace ou d'une clé (aligné sur limite NVS) */
#define MOCK_MAX_KEY_LEN   16

/** Taille maximum d'un blob stocké */
#define MOCK_MAX_DATA_SIZE 4096

/* --- Structure d'une entrée du dictionnaire --- */

typedef struct {
    char     ns[MOCK_MAX_KEY_LEN];    /* Namespace */
    char     key[MOCK_MAX_KEY_LEN];   /* Clé */
    uint8_t  data[MOCK_MAX_DATA_SIZE]; /* Données brutes */
    size_t   len;                      /* Taille des données */
    bool     used;                     /* true si l'entrée est occupée */
} mock_entry_t;

/** Contexte interne du mock : tableau d'entrées */
typedef struct {
    mock_entry_t entries[MOCK_MAX_ENTRIES];
} mock_ctx_t;

/* Contexte statique — un seul mock à la fois suffit pour les tests */
static mock_ctx_t s_mock_ctx;

/* --- Fonctions utilitaires internes --- */

/**
 * Chercher une entrée par namespace + clé.
 * @return Pointeur vers l'entrée si trouvée, NULL sinon.
 */
static mock_entry_t *find_entry(mock_ctx_t *ctx, const char *ns, const char *key)
{
    for (int i = 0; i < MOCK_MAX_ENTRIES; i++) {
        if (ctx->entries[i].used &&
            strncmp(ctx->entries[i].ns, ns, MOCK_MAX_KEY_LEN) == 0 &&
            strncmp(ctx->entries[i].key, key, MOCK_MAX_KEY_LEN) == 0) {
            return &ctx->entries[i];
        }
    }
    return NULL;
}

/**
 * Trouver un emplacement libre dans le tableau.
 * @return Pointeur vers une entrée libre, NULL si plein.
 */
static mock_entry_t *find_free_slot(mock_ctx_t *ctx)
{
    for (int i = 0; i < MOCK_MAX_ENTRIES; i++) {
        if (!ctx->entries[i].used) {
            return &ctx->entries[i];
        }
    }
    return NULL;
}

/* --- Implémentation des opérations de la vtable --- */

/**
 * Écrire un u32 : stocké comme 4 octets dans le blob.
 */
static hal_err_t mock_u32_write(const char *ns, const char *key,
                                uint32_t value, void *ctx)
{
    if (!ns || !key) {
        return HAL_ERR_INVALID;
    }

    mock_ctx_t *mc = (mock_ctx_t *)ctx;

    /* Chercher si l'entrée existe déjà pour la mettre à jour */
    mock_entry_t *entry = find_entry(mc, ns, key);
    if (!entry) {
        /* Nouvelle entrée */
        entry = find_free_slot(mc);
        if (!entry) {
            return HAL_ERR_NO_MEM;
        }
        strncpy(entry->ns, ns, MOCK_MAX_KEY_LEN - 1);
        entry->ns[MOCK_MAX_KEY_LEN - 1] = '\0';
        strncpy(entry->key, key, MOCK_MAX_KEY_LEN - 1);
        entry->key[MOCK_MAX_KEY_LEN - 1] = '\0';
        entry->used = true;
    }

    /* Stocker la valeur u32 comme 4 octets (little-endian natif) */
    memcpy(entry->data, &value, sizeof(uint32_t));
    entry->len = sizeof(uint32_t);

    return HAL_OK;
}

/**
 * Lire un u32 : extrait 4 octets du blob.
 */
static hal_err_t mock_u32_read(const char *ns, const char *key,
                               uint32_t *value, void *ctx)
{
    if (!ns || !key || !value) {
        return HAL_ERR_INVALID;
    }

    mock_ctx_t *mc = (mock_ctx_t *)ctx;
    mock_entry_t *entry = find_entry(mc, ns, key);
    if (!entry) {
        return HAL_ERR_NOT_FOUND;
    }

    if (entry->len != sizeof(uint32_t)) {
        return HAL_ERR_INVALID;
    }

    memcpy(value, entry->data, sizeof(uint32_t));
    return HAL_OK;
}

/**
 * Écrire un blob de données binaires.
 */
static hal_err_t mock_blob_write(const char *ns, const char *key,
                                 const uint8_t *data, size_t len, void *ctx)
{
    if (!ns || !key || !data) {
        return HAL_ERR_INVALID;
    }
    if (len > MOCK_MAX_DATA_SIZE) {
        return HAL_ERR_NO_MEM;
    }

    mock_ctx_t *mc = (mock_ctx_t *)ctx;

    /* Chercher si l'entrée existe déjà */
    mock_entry_t *entry = find_entry(mc, ns, key);
    if (!entry) {
        entry = find_free_slot(mc);
        if (!entry) {
            return HAL_ERR_NO_MEM;
        }
        strncpy(entry->ns, ns, MOCK_MAX_KEY_LEN - 1);
        entry->ns[MOCK_MAX_KEY_LEN - 1] = '\0';
        strncpy(entry->key, key, MOCK_MAX_KEY_LEN - 1);
        entry->key[MOCK_MAX_KEY_LEN - 1] = '\0';
        entry->used = true;
    }

    memcpy(entry->data, data, len);
    entry->len = len;

    return HAL_OK;
}

/**
 * Lire un blob.
 * Si buf est NULL, retourne uniquement la taille dans *len.
 */
static hal_err_t mock_blob_read(const char *ns, const char *key,
                                uint8_t *buf, size_t *len, void *ctx)
{
    if (!ns || !key || !len) {
        return HAL_ERR_INVALID;
    }

    mock_ctx_t *mc = (mock_ctx_t *)ctx;
    mock_entry_t *entry = find_entry(mc, ns, key);
    if (!entry) {
        return HAL_ERR_NOT_FOUND;
    }

    /* Mode requête de taille uniquement */
    if (!buf) {
        *len = entry->len;
        return HAL_OK;
    }

    /* Vérifier que le buffer est assez grand */
    if (*len < entry->len) {
        *len = entry->len; /* Indiquer la taille nécessaire */
        return HAL_ERR_NO_MEM;
    }

    memcpy(buf, entry->data, entry->len);
    *len = entry->len;
    return HAL_OK;
}

/**
 * Supprimer une entrée par namespace + clé.
 */
static hal_err_t mock_erase(const char *ns, const char *key, void *ctx)
{
    if (!ns || !key) {
        return HAL_ERR_INVALID;
    }

    mock_ctx_t *mc = (mock_ctx_t *)ctx;
    mock_entry_t *entry = find_entry(mc, ns, key);
    if (!entry) {
        return HAL_ERR_NOT_FOUND;
    }

    /* Marquer l'entrée comme libre */
    memset(entry, 0, sizeof(mock_entry_t));
    return HAL_OK;
}

/**
 * Vérifier si une clé existe.
 */
static hal_err_t mock_exists(const char *ns, const char *key,
                             bool *exists_out, void *ctx)
{
    if (!ns || !key || !exists_out) {
        return HAL_ERR_INVALID;
    }

    mock_ctx_t *mc = (mock_ctx_t *)ctx;
    *exists_out = (find_entry(mc, ns, key) != NULL);
    return HAL_OK;
}

/* --- API publique --- */

hal_err_t hal_storage_mock_create(hal_storage_t *storage)
{
    if (!storage) {
        return HAL_ERR_INVALID;
    }

    /* Réinitialiser le contexte statique */
    memset(&s_mock_ctx, 0, sizeof(s_mock_ctx));

    /* Remplir la vtable */
    storage->u32_write  = mock_u32_write;
    storage->u32_read   = mock_u32_read;
    storage->blob_write = mock_blob_write;
    storage->blob_read  = mock_blob_read;
    storage->erase      = mock_erase;
    storage->exists     = mock_exists;
    storage->ctx        = &s_mock_ctx;

    return HAL_OK;
}

void hal_storage_mock_reset(hal_storage_t *storage)
{
    if (!storage || !storage->ctx) {
        return;
    }
    memset(storage->ctx, 0, sizeof(mock_ctx_t));
}

uint32_t hal_storage_mock_count(const hal_storage_t *storage)
{
    if (!storage || !storage->ctx) {
        return 0;
    }

    const mock_ctx_t *mc = (const mock_ctx_t *)storage->ctx;
    uint32_t count = 0;
    for (int i = 0; i < MOCK_MAX_ENTRIES; i++) {
        if (mc->entries[i].used) {
            count++;
        }
    }
    return count;
}
