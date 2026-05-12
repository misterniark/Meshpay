/**
 * @file hal_storage.h
 * @brief Interface abstraite de stockage persistant (HAL).
 *
 * Définit une vtable de 6 opérations pour lire/écrire des données
 * persistantes, organisées par namespace et clé (modèle key-value).
 *
 * Deux types de données sont supportés :
 * - u32 : entiers 32 bits (flags, compteurs, config)
 * - blob : données binaires brutes (keypairs, checkpoints)
 *
 * Chaque implémentation (NVS ESP32, Flash STM32, mock mémoire)
 * fournit une fonction factory qui remplit la vtable.
 *
 * Contraintes :
 * - namespace : 15 caractères max (limite NVS ESP-IDF)
 * - clé : 15 caractères max
 * - Les implémentations DOIVENT être thread-safe.
 *
 * Portabilité : ce header n'inclut aucun header spécifique plateforme.
 */

#ifndef HAL_STORAGE_H
#define HAL_STORAGE_H

#include "hal/hal_types.h"

/**
 * Vtable de stockage persistant.
 *
 * Chaque pointeur de fonction reçoit le contexte opaque `ctx` en
 * dernier paramètre. Ce contexte contient les données internes de
 * l'implémentation (handle NVS, dictionnaire mock, etc.).
 */
typedef struct {
    /**
     * Écrire un entier 32 bits.
     *
     * @param ns    Namespace (max 15 chars)
     * @param key   Clé (max 15 chars)
     * @param value Valeur à écrire
     * @param ctx   Contexte opaque de l'implémentation
     * @return HAL_OK en cas de succès, code erreur sinon
     */
    hal_err_t (*u32_write)(const char *ns, const char *key,
                           uint32_t value, void *ctx);

    /**
     * Lire un entier 32 bits.
     *
     * @param ns    Namespace
     * @param key   Clé
     * @param value [out] Valeur lue
     * @param ctx   Contexte opaque
     * @return HAL_OK ou HAL_ERR_NOT_FOUND si la clé n'existe pas
     */
    hal_err_t (*u32_read)(const char *ns, const char *key,
                          uint32_t *value, void *ctx);

    /**
     * Écrire des données binaires (blob).
     *
     * @param ns   Namespace
     * @param key  Clé
     * @param data Pointeur vers les données à écrire
     * @param len  Taille en octets
     * @param ctx  Contexte opaque
     * @return HAL_OK en cas de succès
     */
    hal_err_t (*blob_write)(const char *ns, const char *key,
                            const uint8_t *data, size_t len, void *ctx);

    /**
     * Lire des données binaires (blob).
     *
     * Si buf est NULL, seule la taille est retournée dans *len.
     * Cela permet au appelant de connaître la taille avant d'allouer.
     *
     * @param ns  Namespace
     * @param key Clé
     * @param buf [out] Buffer de destination (NULL pour requête taille)
     * @param len [in/out] En entrée : taille du buffer. En sortie : taille lue.
     * @param ctx Contexte opaque
     * @return HAL_OK, HAL_ERR_NOT_FOUND, ou HAL_ERR_NO_MEM si buffer trop petit
     */
    hal_err_t (*blob_read)(const char *ns, const char *key,
                           uint8_t *buf, size_t *len, void *ctx);

    /**
     * Supprimer une entrée.
     *
     * @param ns  Namespace
     * @param key Clé
     * @param ctx Contexte opaque
     * @return HAL_OK ou HAL_ERR_NOT_FOUND
     */
    hal_err_t (*erase)(const char *ns, const char *key, void *ctx);

    /**
     * Vérifier l'existence d'une clé.
     *
     * @param ns     Namespace
     * @param key    Clé
     * @param exists [out] true si la clé existe, false sinon
     * @param ctx    Contexte opaque
     * @return HAL_OK en cas de succès
     */
    hal_err_t (*exists)(const char *ns, const char *key,
                        bool *exists, void *ctx);

    /** Contexte opaque de l'implémentation (NVS handle, mock dict, etc.) */
    void *ctx;
} hal_storage_t;

#endif /* HAL_STORAGE_H */
