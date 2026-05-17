/**
 * @file ui_pin.c
 * @brief Logique PIN : hashage, stockage NVS, verification, anti brute-force.
 *
 * Le PIN est un code a 4 chiffres (0-9). Il est hashe via
 * PBKDF2-HMAC-SHA256 (10 000 iterations) avec un sel aleatoire
 * de 16 octets avant stockage dans le NVS. La verification
 * relit le sel et recalcule le hash pour comparaison.
 *
 * Protection anti brute-force :
 *   - 1-2 echecs : pas de delai
 *   - 3 echecs   : 30 secondes
 *   - 5 echecs   : 5 minutes
 *   - 10 echecs  : blocage total (reset NVS necessaire)
 *
 * Le compteur d'echecs est persiste en NVS (survit au redemarrage).
 *
 * Blacklist de PIN faibles :
 *   0000, 1111, 2222, ..., 9999 (repetitions)
 *   1234, 5678, 4321, 8765 (suites)
 */

#include "ui/ui_pin.h"
#include "crypto/crypto_hash.h"
#include "crypto/crypto_types.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "mbedtls/pkcs5.h"
#include "mbedtls/md.h"

#include <string.h>

static const char *TAG = "ui_pin";

/* ================================================================
 * Cles NVS
 * ================================================================ */

/** Hash PBKDF2-HMAC-SHA256 du PIN (blob de 32 octets) */
#define NVS_KEY_PIN_HASH    "pin_hash"
/** Sel aleatoire de 16 octets pour le hachage du PIN (blob) */
#define NVS_KEY_PIN_SALT    "pin_salt"
/** Nombre de tentatives echouees consecutives (u32) */
#define NVS_KEY_FAIL_COUNT  "pin_fails"
/** Timestamp (esp_timer, en us) de la derniere tentative echouee (blob u64) */
#define NVS_KEY_FAIL_TIME   "pin_fail_t"

/** Taille du sel en octets */
#define PIN_SALT_SIZE       16
/** Nombre d'iterations PBKDF2 — cout suffisant pour ralentir le brute-force
 *  des 10 000 combinaisons PIN a 4 chiffres */
#define PIN_PBKDF2_ITERATIONS  10000

/* ================================================================
 * Seuils anti brute-force
 * ================================================================ */

/** Nombre d'echecs a partir duquel un delai de 30s est impose */
#define BRUTE_FORCE_DELAY_1_THRESHOLD   3
#define BRUTE_FORCE_DELAY_1_SECONDS     30

/** Nombre d'echecs a partir duquel un delai de 5 minutes est impose */
#define BRUTE_FORCE_DELAY_2_THRESHOLD   5
#define BRUTE_FORCE_DELAY_2_SECONDS     300

/** Nombre d'echecs a partir duquel le device est bloque */
#define BRUTE_FORCE_BLOCK_THRESHOLD     10

/* ================================================================
 * Overlay LVGL
 * ================================================================ */

/** Overlay PIN actif (pour pouvoir le fermer) */
static lv_obj_t *s_pin_overlay = NULL;

/* ================================================================
 * Blacklist de PIN faibles
 * ================================================================ */

/**
 * Liste des PIN interdits.
 * Chaque entree est un tableau de 4 chiffres.
 */
static const uint8_t s_weak_pins[][UI_PIN_LENGTH] = {
    /* Repetitions : 0000, 1111, ..., 9999 */
    {0,0,0,0}, {1,1,1,1}, {2,2,2,2}, {3,3,3,3}, {4,4,4,4},
    {5,5,5,5}, {6,6,6,6}, {7,7,7,7}, {8,8,8,8}, {9,9,9,9},
    /* Suites croissantes */
    {0,1,2,3}, {1,2,3,4}, {2,3,4,5}, {3,4,5,6}, {4,5,6,7},
    {5,6,7,8}, {6,7,8,9},
    /* Suites decroissantes (completees) */
    {9,8,7,6}, {8,7,6,5}, {7,6,5,4}, {6,5,4,3}, {5,4,3,2}, {4,3,2,1},
    {3,2,1,0},
    /* Annees courantes et recentes */
    {2,0,2,4}, {2,0,2,5}, {2,0,2,6}, {2,0,0,0}, {1,9,9,9},
    /* Patterns pave numerique */
    {1,3,5,7}, {2,4,6,8}, {0,8,5,2},
    /* Motifs courants */
    {0,0,0,1}, {1,0,0,0}, {1,2,1,2}, {0,0,1,1},
};

#define WEAK_PIN_COUNT (sizeof(s_weak_pins) / sizeof(s_weak_pins[0]))

/* ================================================================
 * Fonctions utilitaires
 * ================================================================ */

/**
 * Convertit un PIN (4 chiffres 0-9) en tableau d'octets ASCII
 * pour le hachage. Le format est "XYZW" (4 octets ASCII).
 */
static void pin_to_bytes(const uint8_t pin[UI_PIN_LENGTH], uint8_t out[UI_PIN_LENGTH])
{
    for (int i = 0; i < UI_PIN_LENGTH; i++) {
        out[i] = '0' + pin[i];
    }
}

/**
 * Calcule le hash PBKDF2-HMAC-SHA256 d'un PIN avec un sel.
 *
 * PBKDF2 avec 10 000 iterations rend le brute-force des 10 000
 * combinaisons possibles significativement plus couteux qu'un simple
 * SHA-256, meme si l'attaquant connait le sel.
 *
 * @param pin   PIN a 4 chiffres (valeurs 0-9)
 * @param salt  Sel aleatoire de PIN_SALT_SIZE octets
 * @param hash  [out] Hash resultant (32 octets)
 * @return ESP_OK en cas de succes
 */
static esp_err_t pin_hash(const uint8_t pin[UI_PIN_LENGTH],
                           const uint8_t salt[PIN_SALT_SIZE],
                           hash_t *hash)
{
    uint8_t ascii[UI_PIN_LENGTH];
    pin_to_bytes(pin, ascii);

    /* PBKDF2-HMAC-SHA256 avec sel — API _ext (mbedtls 3.x) */
    int ret = mbedtls_pkcs5_pbkdf2_hmac_ext(MBEDTLS_MD_SHA256,
                                             ascii, UI_PIN_LENGTH,
                                             salt, PIN_SALT_SIZE,
                                             PIN_PBKDF2_ITERATIONS,
                                             sizeof(hash->bytes), hash->bytes);

    if (ret != 0) {
        ESP_LOGE(TAG, "Erreur PBKDF2: %d", ret);
        return ESP_FAIL;
    }

    return ESP_OK;
}

/**
 * Lit le compteur d'echecs depuis le NVS.
 * Retourne 0 si la cle n'existe pas.
 */
static uint32_t read_fail_count(hal_storage_t *storage)
{
    uint32_t count = 0;
    if (storage->u32_read(UI_PIN_NVS_NS, NVS_KEY_FAIL_COUNT,
                          &count, storage->ctx) != HAL_OK) {
        return 0;
    }
    return count;
}

/**
 * Ecrit le compteur d'echecs dans le NVS.
 */
static void write_fail_count(hal_storage_t *storage, uint32_t count)
{
    storage->u32_write(UI_PIN_NVS_NS, NVS_KEY_FAIL_COUNT,
                       count, storage->ctx);
}

/**
 * Lit le timestamp de la derniere tentative echouee (en microsecondes).
 */
static int64_t read_fail_time(hal_storage_t *storage)
{
    int64_t t = 0;
    size_t len = sizeof(t);
    if (storage->blob_read(UI_PIN_NVS_NS, NVS_KEY_FAIL_TIME,
                           (uint8_t *)&t, &len, storage->ctx) != HAL_OK) {
        return 0;
    }
    return t;
}

/**
 * Ecrit le timestamp de la derniere tentative echouee.
 */
static void write_fail_time(hal_storage_t *storage, int64_t t)
{
    storage->blob_write(UI_PIN_NVS_NS, NVS_KEY_FAIL_TIME,
                        (const uint8_t *)&t, sizeof(t), storage->ctx);
}

/**
 * Retourne le delai requis (en secondes) en fonction du nombre d'echecs.
 * 0 = pas de delai. UINT32_MAX = bloque.
 */
static uint32_t get_required_delay(uint32_t fail_count)
{
    if (fail_count >= BRUTE_FORCE_BLOCK_THRESHOLD) {
        return UINT32_MAX; /* Bloque */
    }
    if (fail_count >= BRUTE_FORCE_DELAY_2_THRESHOLD) {
        return BRUTE_FORCE_DELAY_2_SECONDS;
    }
    if (fail_count >= BRUTE_FORCE_DELAY_1_THRESHOLD) {
        return BRUTE_FORCE_DELAY_1_SECONDS;
    }
    return 0;
}

/* ================================================================
 * API publique
 * ================================================================ */

bool ui_pin_is_weak(const uint8_t pin[UI_PIN_LENGTH])
{
    for (size_t i = 0; i < WEAK_PIN_COUNT; i++) {
        if (memcmp(pin, s_weak_pins[i], UI_PIN_LENGTH) == 0) {
            return true;
        }
    }
    return false;
}

bool ui_pin_is_configured(hal_storage_t *storage)
{
    bool exists = false;
    if (storage->exists(UI_PIN_NVS_NS, NVS_KEY_PIN_HASH,
                        &exists, storage->ctx) != HAL_OK) {
        return false;
    }
    return exists;
}

ui_pin_result_t ui_pin_register(const uint8_t pin[UI_PIN_LENGTH],
                                hal_storage_t *storage)
{
    /* Verifier la blacklist */
    if (ui_pin_is_weak(pin)) {
        ESP_LOGW(TAG, "PIN rejete : code trop faible");
        return UI_PIN_WEAK;
    }

    /* Generer un sel aleatoire de 16 octets via le TRNG de l'ESP32 */
    uint8_t salt[PIN_SALT_SIZE];
    esp_fill_random(salt, PIN_SALT_SIZE);

    /* Hasher le PIN avec le sel via PBKDF2 */
    hash_t h;
    if (pin_hash(pin, salt, &h) != ESP_OK) {
        ESP_LOGE(TAG, "Erreur hachage PIN");
        return UI_PIN_WRONG;
    }

    /* Stocker le sel dans NVS */
    hal_err_t ret = storage->blob_write(UI_PIN_NVS_NS, NVS_KEY_PIN_SALT,
                                        salt, PIN_SALT_SIZE,
                                        storage->ctx);
    if (ret != HAL_OK) {
        ESP_LOGE(TAG, "Erreur stockage sel PIN en NVS");
        return UI_PIN_WRONG;
    }

    /* Stocker le hash dans NVS */
    ret = storage->blob_write(UI_PIN_NVS_NS, NVS_KEY_PIN_HASH,
                              h.bytes, sizeof(h.bytes),
                              storage->ctx);
    if (ret != HAL_OK) {
        ESP_LOGE(TAG, "Erreur stockage hash PIN en NVS");
        return UI_PIN_WRONG;
    }

    /* Reinitialiser le compteur d'echecs */
    write_fail_count(storage, 0);

    ESP_LOGI(TAG, "PIN enregistre avec succes (PBKDF2 + sel)");
    return UI_PIN_OK;
}

ui_pin_result_t ui_pin_verify(const uint8_t pin[UI_PIN_LENGTH],
                              hal_storage_t *storage)
{
    /* 1. Verifier le blocage / cooldown */
    uint32_t fail_count = read_fail_count(storage);
    uint32_t required_delay = get_required_delay(fail_count);

    if (required_delay == UINT32_MAX) {
        ESP_LOGW(TAG, "Device bloque apres %"PRIu32" echecs", fail_count);
        return UI_PIN_BLOCKED;
    }

    if (required_delay > 0) {
        int64_t last_fail = read_fail_time(storage);
        int64_t now = esp_timer_get_time(); /* microsecondes, monotone depuis boot */

        if (now < last_fail) {
            /*
             * [F-UI-007] Décision design 2026-05-16 : `now < last_fail`
             * indique un reboot (esp_timer repart à zéro). On PÉNALISE
             * le reboot en redémarrant le cooldown complet, au lieu de
             * le considérer expiré (ancien fix B4).
             *
             * Justification : sans cette pénalité, un attaquant pouvait
             * bypasser le délai anti brute-force de 30 s en rebootant
             * physiquement le device après 3 essais. Avec ce fix, le
             * reboot écrase `last_fail` avec le nouveau `now` et le
             * cooldown complet recommence — pénalité légère pour
             * l'utilisateur légitime qui reboot par erreur, bloquante
             * pour l'attaquant.
             */
            ESP_LOGW(TAG, "Timestamp NVS posterieur a esp_timer (reboot ?) — "
                          "redemarrage du cooldown complet (anti bypass)");
            write_fail_time(storage, now);
            return UI_PIN_COOLDOWN;
        }
        int64_t elapsed_s = (now - last_fail) / 1000000;
        if (elapsed_s < (int64_t)required_delay) {
            ESP_LOGW(TAG, "Cooldown actif: %"PRId64"s restantes",
                     (int64_t)required_delay - elapsed_s);
            return UI_PIN_COOLDOWN;
        }
    }

    /* 2. Lire le sel depuis NVS */
    uint8_t salt[PIN_SALT_SIZE];
    size_t salt_len = PIN_SALT_SIZE;
    hal_err_t ret = storage->blob_read(UI_PIN_NVS_NS, NVS_KEY_PIN_SALT,
                                       salt, &salt_len, storage->ctx);
    if (ret != HAL_OK || salt_len != PIN_SALT_SIZE) {
        ESP_LOGE(TAG, "Sel PIN absent ou invalide en NVS");
        return UI_PIN_WRONG;
    }

    /* 3. Lire le hash stocke */
    hash_t stored_hash;
    size_t len = sizeof(stored_hash.bytes);
    ret = storage->blob_read(UI_PIN_NVS_NS, NVS_KEY_PIN_HASH,
                             stored_hash.bytes, &len,
                             storage->ctx);
    if (ret != HAL_OK || len != sizeof(stored_hash.bytes)) {
        ESP_LOGE(TAG, "PIN non configure ou erreur lecture NVS");
        return UI_PIN_WRONG;
    }

    /* 4. Hasher le PIN saisi avec le meme sel et comparer */
    hash_t input_hash;
    if (pin_hash(pin, salt, &input_hash) != ESP_OK) {
        return UI_PIN_WRONG;
    }

    if (hash_equal(&stored_hash, &input_hash)) {
        /* PIN correct : reinitialiser le compteur */
        write_fail_count(storage, 0);
        ESP_LOGI(TAG, "PIN correct");
        return UI_PIN_OK;
    }

    /* 5. PIN incorrect : incrementer le compteur d'echecs */
    fail_count++;
    write_fail_count(storage, fail_count);
    write_fail_time(storage, esp_timer_get_time());

    uint32_t next_delay = get_required_delay(fail_count);
    if (next_delay == UINT32_MAX) {
        ESP_LOGE(TAG, "PIN incorrect (%"PRIu32" echecs) — DEVICE BLOQUE",
                 fail_count);
        return UI_PIN_BLOCKED;
    } else if (next_delay > 0) {
        ESP_LOGW(TAG, "PIN incorrect (%"PRIu32" echecs) — delai %"PRIu32"s",
                 fail_count, next_delay);
    } else {
        ESP_LOGW(TAG, "PIN incorrect (%"PRIu32" echecs)", fail_count);
    }

    return UI_PIN_WRONG;
}

uint32_t ui_pin_cooldown_remaining(hal_storage_t *storage)
{
    uint32_t fail_count = read_fail_count(storage);
    uint32_t required_delay = get_required_delay(fail_count);

    if (required_delay == 0 || required_delay == UINT32_MAX) {
        return required_delay;
    }

    int64_t last_fail = read_fail_time(storage);
    int64_t now = esp_timer_get_time();

    /* Meme politique que ui_pin_verify : si le timestamp NVS est posterieur
       a l'horloge monotone, on considere qu'un reboot est intervenu et on
       penalise avec le cooldown complet pour eviter le bypass par reboot. */
    if (now < last_fail) {
        return required_delay;
    }

    int64_t elapsed_s = (now - last_fail) / 1000000;
    if (elapsed_s >= (int64_t)required_delay) {
        return 0;
    }
    return (uint32_t)((int64_t)required_delay - elapsed_s);
}

/* ================================================================
 * Dispatcher LVGL (choix numpad vs ledger)
 * ================================================================ */

lv_obj_t *ui_pin_show(lv_obj_t *parent, const char *title,
                       ui_pin_callback_t callback, void *user_data,
                       bool is_small)
{
    /*
     * [F-UI-004] Garde contre la réentrance : si un overlay est déjà
     * ouvert, on retourne le même handle sans en créer un second.
     * Sans cette garde, un double tap rapide sur "Admin" pouvait
     * appeler ui_pin_show deux fois ; le second appel `memset` les
     * singletons statiques `s_numpad_ctx` / `s_ledger_ctx` et créait
     * des dangling pointers vers des widgets LVGL détruits → crash
     * ou perte silencieuse de la saisie.
     */
    if (s_pin_overlay != NULL) {
        return s_pin_overlay;
    }

    if (is_small) {
        s_pin_overlay = ui_pin_ledger_create(parent, title, callback, user_data);
    } else {
        s_pin_overlay = ui_pin_numpad_create(parent, title, callback, user_data);
    }

    return s_pin_overlay;
}

void ui_pin_close(void)
{
    if (s_pin_overlay) {
        lv_obj_delete(s_pin_overlay);
        s_pin_overlay = NULL;
    }
}
