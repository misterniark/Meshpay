# Gestion de l'énergie (Phase 1) — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Poser l'infrastructure de gestion d'énergie du firmware Mesh Pay : une machine d'états ACTIF/ÉCO sur le Waveshare ESP32-S3, déclenchée par inactivité, qui éteint le backlight et active le CPU frequency scaling — inerte tant que le hardware batterie n'est pas câblé.

**Architecture:** Une HAL `hal_power` (abstraction source d'alim, impl stub « toujours USB ») + un module `power_manager` (machine d'états, dépendance-pure, injection de dépendances). Pattern facade+stub sélectionné par CMake : `power_manager.c` (réel, ESP32-S3) vs `power_manager_stub.c` (no-op, CYD). Intégration via callbacks dans `app_main`, `core_task`, `ui_task`. Aucune nouvelle tâche FreeRTOS.

**Tech Stack:** C / ESP-IDF v5.4.3, Unity (tests on-target via `test_app`), `esp_pm` (frequency scaling), pattern HAL existant.

**Spec de référence :** `/Users/misterniark/Documents/Obsidian/Misterniark/Projet/Mesh Pay/Doctech/13 - Gestion de l'énergie (design).md`

---

## Notes d'exécution

- **Répertoire de travail :** worktree `/Users/misterniark/Library/CloudStorage/Dropbox/Code/Mesh Pay/.claude/worktrees/vigilant-turing-4dc431`
- **Environnement ESP-IDF :** chaque commande `idf.py` doit être précédée de `source ~/.espressif/v5.4.3/esp-idf/export.sh >/dev/null 2>&1 &&`
- **Build firmware ESP32-S3 :** `idf.py --no-hints -B build-s3 build` (supprimer `sdkconfig` avant si la cible a changé)
- **Build firmware ESP32 (CYD) :** `idf.py --no-hints -B build-esp32 -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.esp32" build`
- **Build de l'app de test :** `idf.py -C test_app -B test_app/build-s3 set-target esp32s3` puis `idf.py -C test_app -B test_app/build-s3 build`
- **Les tests Unity tournent on-target** : `power_manager.c` est dépendance-pure (deps injectées), donc les 6 cas de test n'ont besoin d'aucun hardware batterie, mais ils s'exécutent quand même via le menu Unity série du `test_app` flashé sur le S3. Dans ce plan, « vérifier que le test échoue/passe » au niveau compilation = `idf.py -C test_app build`. La validation des assertions Unity se fait au flash final (Task 8).
- **Commits :** un commit par task, message en français, préfixe `feat(power):` ou `test(power):`.

---

## Task 1 : HAL `hal_power` — interface + stub

Crée l'abstraction de la source d'alimentation. Le stub renvoie toujours `POWER_SOURCE_USB` (pas de hardware batterie aujourd'hui). Compilé pour toutes les cibles.

**Files:**
- Create: `components/device_hal/include/hal/hal_power.h`
- Create: `components/device_hal/src/hal_power_stub.c`
- Create: `components/device_hal/test/test_hal_power.c`
- Modify: `components/device_hal/CMakeLists.txt`

- [ ] **Step 1 : Écrire le header de l'interface HAL**

Create `components/device_hal/include/hal/hal_power.h` :

```c
/**
 * @file hal_power.h
 * @brief Interface abstraite de la source d'alimentation (HAL).
 *
 * Permet au firmware de savoir s'il tourne sur USB ou sur batterie,
 * sans connaitre le mecanisme materiel de detection (GPIO, ADC...).
 *
 * Une seule operation : get_source(). Chaque implementation fournit
 * une fonction factory qui remplit la vtable.
 *
 * Etat actuel : le hardware batterie n'existe pas encore. La seule impl
 * disponible est hal_power_stub.c qui renvoie toujours POWER_SOURCE_USB.
 * Une vraie impl lira un GPIO/ADC quand la carte batterie sera prete.
 *
 * Portabilite : ce header n'inclut aucun header specifique plateforme.
 */

#ifndef HAL_POWER_H
#define HAL_POWER_H

#include "hal/hal_types.h"

/**
 * Source d'alimentation du device.
 */
typedef enum {
    POWER_SOURCE_USB,      /* Alimente en USB / secteur */
    POWER_SOURCE_BATTERY,  /* Sur batterie */
    POWER_SOURCE_UNKNOWN,  /* Indetermine — a traiter comme BATTERY (prudent) */
} hal_power_source_t;

/**
 * Vtable de la source d'alimentation.
 */
typedef struct hal_power_s {
    /**
     * Retourne la source d'alimentation courante.
     * DOIT etre thread-safe.
     *
     * @param ctx Contexte opaque
     * @return La source d'alimentation detectee
     */
    hal_power_source_t (*get_source)(void *ctx);

    /** Contexte opaque passe a get_source(). */
    void *ctx;
} hal_power_t;

/**
 * @brief Factory de l'implementation stub (toujours POWER_SOURCE_USB).
 *
 * A remplacer par une vraie factory (GPIO/ADC) quand le hardware
 * batterie sera disponible.
 *
 * @param out Vtable a remplir
 * @return HAL_OK
 */
hal_err_t hal_power_stub_create(hal_power_t *out);

#endif /* HAL_POWER_H */
```

- [ ] **Step 2 : Écrire le test (échec attendu : symboles non définis)**

Create `components/device_hal/test/test_hal_power.c` :

```c
/**
 * @file test_hal_power.c
 * @brief Tests de l'impl stub de hal_power.
 */

#include "unity.h"
#include "hal/hal_power.h"

TEST_CASE("hal_power_stub_create_returns_ok", "[hal_power]")
{
    hal_power_t power;
    hal_err_t err = hal_power_stub_create(&power);
    TEST_ASSERT_EQUAL(HAL_OK, err);
    TEST_ASSERT_NOT_NULL(power.get_source);
}

TEST_CASE("hal_power_stub_always_usb", "[hal_power]")
{
    hal_power_t power;
    hal_power_stub_create(&power);
    TEST_ASSERT_EQUAL(POWER_SOURCE_USB, power.get_source(power.ctx));
}
```

- [ ] **Step 3 : Brancher le test dans le CMakeLists du composant**

Modify `components/device_hal/CMakeLists.txt`. Ajouter `src/hal_power_stub.c` à `HAL_SRCS` de façon inconditionnelle (avant le bloc `if(CONFIG_IDF_TARGET_ESP32)`), et ajouter `test/test_hal_power.c` à la liste de test :

Remplacer le début du fichier :
```cmake
set(HAL_SRCS "")
set(HAL_PRIV_INCLUDE "")
```
par :
```cmake
set(HAL_SRCS "src/hal_power_stub.c")
set(HAL_PRIV_INCLUDE "")
```

Et dans le bloc `if(project_name STREQUAL "meshpay_test_app")`, ajouter `test/test_hal_power.c` à la liste existante :
```cmake
    list(APPEND HAL_SRCS
        "src/mock/hal_storage_mock.c"
        "src/mock/hal_display_mock.c"
        "src/mock/hal_lora_mock.c"
        "test/test_hal_storage.c"
        "test/test_hal_power.c"
    )
```

- [ ] **Step 4 : Vérifier que le build test_app échoue (impl manquante)**

Run :
```bash
source ~/.espressif/v5.4.3/esp-idf/export.sh >/dev/null 2>&1 && idf.py -C test_app -B test_app/build-s3 set-target esp32s3 && idf.py -C test_app -B test_app/build-s3 build
```
Expected : ÉCHEC au link — `undefined reference to 'hal_power_stub_create'`.

- [ ] **Step 5 : Écrire l'implémentation stub**

Create `components/device_hal/src/hal_power_stub.c` :

```c
/**
 * @file hal_power_stub.c
 * @brief Implementation stub de hal_power — renvoie toujours USB.
 *
 * Le hardware de detection de la source d'alimentation n'existe pas
 * encore. Cette impl est inoffensive : elle declare que le device est
 * toujours sur USB, ce qui maintient le firmware en mode pleine
 * puissance (la machine power_manager ne passe jamais en ECO sur USB).
 *
 * A remplacer par une vraie factory (lecture GPIO/ADC) quand la carte
 * batterie sera prete.
 */

#include "hal/hal_power.h"

/* Pas de contexte necessaire : la reponse est constante. */
static hal_power_source_t stub_get_source(void *ctx)
{
    (void)ctx;
    return POWER_SOURCE_USB;
}

hal_err_t hal_power_stub_create(hal_power_t *out)
{
    if (out == NULL) {
        return HAL_ERR_INVALID_ARG;
    }
    out->get_source = stub_get_source;
    out->ctx = NULL;
    return HAL_OK;
}
```

> **Note :** vérifier que `HAL_ERR_INVALID_ARG` existe dans `hal_types.h`. Si le nom diffère (ex. `HAL_ERR_INVALID`), utiliser le nom réel — lire `components/device_hal/include/hal/hal_types.h` pour confirmer.

- [ ] **Step 6 : Vérifier que le build test_app passe**

Run :
```bash
source ~/.espressif/v5.4.3/esp-idf/export.sh >/dev/null 2>&1 && idf.py -C test_app -B test_app/build-s3 build
```
Expected : `Project build complete`.

- [ ] **Step 7 : Commit**

```bash
git add components/device_hal/include/hal/hal_power.h \
        components/device_hal/src/hal_power_stub.c \
        components/device_hal/test/test_hal_power.c \
        components/device_hal/CMakeLists.txt
git commit -m "feat(power): ajouter la HAL hal_power + impl stub (toujours USB)"
```

---

## Task 2 : `power_manager` — header + squelette + composant de test + Test 1

Crée le module `power_manager` (header public + impl réelle squelette) et le composant de test `test_app/components/test_power_manager/`. Le composant de test compile `../../../main/power_manager.c` directement (possible car `power_manager.c` est dépendance-pure). Premier test : sur USB, jamais d'ÉCO.

**Files:**
- Create: `main/power_manager.h`
- Create: `main/power_manager.c`
- Create: `test_app/components/test_power_manager/CMakeLists.txt`
- Create: `test_app/components/test_power_manager/test_power_manager.c`

- [ ] **Step 1 : Écrire le header public**

Create `main/power_manager.h` :

```c
/**
 * @file power_manager.h
 * @brief Machine d'etats de gestion de l'energie (ACTIF / ECO).
 *
 * Sur le Waveshare ESP32-S3 (impl reelle power_manager.c), gere la
 * transition entre un mode ACTIF (comportement normal) et un mode ECO
 * (backlight eteint + CPU frequency scaling), declenchee par inactivite.
 * Sur le CYD (power_manager_stub.c), tout est no-op.
 *
 * CONTRAINTE D'ISOLATION : power_manager.c n'inclut JAMAIS app_state.h
 * ni aucun header interne de main/. Toutes ses dependances passent par
 * le power_manager_config_t injecte a l'init. Il ne depend que de son
 * propre header + hal/hal_power.h + <stdint.h>/<stdbool.h>. Cela le rend
 * compilable en isolation pour ses tests natifs.
 */

#ifndef MESHPAY_POWER_MANAGER_H
#define MESHPAY_POWER_MANAGER_H

#include <stdint.h>
#include <stdbool.h>
#include "hal/hal_power.h"

/** Timeout d'inactivite avant passage en ECO (ms), sur batterie. */
#define POWER_ECO_TIMEOUT_MS 120000u

/**
 * Etats de la machine d'energie.
 */
typedef enum {
    POWER_STATE_ACTIF,  /* Comportement normal : backlight 100, freq max */
    POWER_STATE_ECO,    /* Eco : backlight 0, CPU frequency scaling actif */
} power_state_t;

/**
 * Configuration injectee a l'init.
 *
 * Tous les champs sont des callbacks/valeurs fournis par l'appelant —
 * c'est ce qui rend power_manager.c testable en isolation.
 */
typedef struct {
    /** Source de temps monotone en ms (firmware : get_time_ms_wrapper). */
    uint64_t (*get_time_ms)(void);

    /** Lit la source d'alimentation courante (firmware : hal_power). */
    hal_power_source_t (*get_power_source)(void);

    /** Regle le backlight : 0 = eteint, 100 = max (firmware : hal_display). */
    void (*set_backlight)(uint8_t pct);

    /** Applique la config esp_pm correspondant a l'etat (firmware : esp_pm). */
    void (*apply_pm_config)(power_state_t state);

    /** Verrou applicatif : protege l'etat partage entre core_task et ui_task.
     *  Firmware : mutex FreeRTOS dedie. Test (mono-thread) : no-op. */
    void (*lock)(void);
    void (*unlock)(void);

    /** Timeout d'inactivite en ms. 0 => POWER_ECO_TIMEOUT_MS. */
    uint32_t eco_timeout_ms;
} power_manager_config_t;

/**
 * @brief Initialise la machine d'energie. Etat initial : ACTIF.
 *
 * Ne touche pas au backlight ni a esp_pm : app_main a deja applique la
 * config de boot (ACTIF). Copie cfg en interne.
 */
void power_manager_init(const power_manager_config_t *cfg);

/**
 * @brief Signale une interaction (touch, evenement reseau, commande UI).
 *
 * Met a jour l'horodatage de derniere activite et, si on etait en ECO,
 * repasse immediatement en ACTIF. Thread-safe (utilise lock/unlock).
 */
void power_manager_notify_activity(void);

/**
 * @brief A appeler periodiquement (firmware : chaque tour de core_task).
 *
 * Relit la source d'alimentation et decide la transition ACTIF -> ECO
 * si le device est sur batterie et inactif depuis >= eco_timeout_ms.
 * Sur USB : force le retour en ACTIF. Thread-safe.
 */
void power_manager_tick(void);

/**
 * @brief Retourne l'etat courant.
 */
power_state_t power_manager_get_state(void);

#endif /* MESHPAY_POWER_MANAGER_H */
```

- [ ] **Step 2 : Écrire l'impl réelle (squelette complet — toute la logique)**

Create `main/power_manager.c` :

```c
/**
 * @file power_manager.c
 * @brief Implementation reelle de la machine d'energie (ESP32-S3).
 *
 * Compile uniquement sur ESP32-S3 (cf. main/CMakeLists.txt). Sur le CYD
 * c'est power_manager_stub.c qui est lie.
 *
 * Dependance-pure : aucun include de app_state.h ni de header IDF. Tout
 * passe par le power_manager_config_t injecte.
 */

#include "power_manager.h"

/* Etat interne du module (singleton). */
static power_manager_config_t s_cfg;
static power_state_t          s_state;
static uint32_t               s_last_activity_ms;  /* tronque de get_time_ms */
static uint32_t               s_timeout_ms;

/**
 * Applique une transition d'etat. DOIT etre appele sous lock().
 * Idempotent : si l'etat ne change pas, ne fait rien.
 */
static void enter_state_locked(power_state_t st)
{
    if (st == s_state) {
        return;
    }
    s_state = st;
    if (st == POWER_STATE_ECO) {
        s_cfg.set_backlight(0);
        s_cfg.apply_pm_config(POWER_STATE_ECO);
    } else {
        s_cfg.set_backlight(100);
        s_cfg.apply_pm_config(POWER_STATE_ACTIF);
    }
}

void power_manager_init(const power_manager_config_t *cfg)
{
    s_cfg        = *cfg;
    s_timeout_ms = (cfg->eco_timeout_ms != 0u) ? cfg->eco_timeout_ms
                                               : POWER_ECO_TIMEOUT_MS;
    s_state            = POWER_STATE_ACTIF;
    s_last_activity_ms = (uint32_t)s_cfg.get_time_ms();
    /* Pas de set_backlight/apply_pm_config ici : app_main a deja applique
     * la config de boot (ACTIF). */
}

void power_manager_notify_activity(void)
{
    s_cfg.lock();
    s_last_activity_ms = (uint32_t)s_cfg.get_time_ms();
    enter_state_locked(POWER_STATE_ACTIF);
    s_cfg.unlock();
}

void power_manager_tick(void)
{
    hal_power_source_t src = s_cfg.get_power_source();

    s_cfg.lock();
    if (src == POWER_SOURCE_USB) {
        /* Sur USB : jamais d'ECO. Remonter si on y etait (source changee
         * a chaud, ex. branchement USB pendant l'ECO). */
        enter_state_locked(POWER_STATE_ACTIF);
    } else if (s_state == POWER_STATE_ACTIF) {
        /* BATTERY ou UNKNOWN : ECO apres timeout d'inactivite.
         * Soustraction wraparound-safe sur uint32_t. */
        uint32_t now  = (uint32_t)s_cfg.get_time_ms();
        uint32_t idle = now - s_last_activity_ms;
        if (idle >= s_timeout_ms) {
            enter_state_locked(POWER_STATE_ECO);
        }
    }
    s_cfg.unlock();
}

power_state_t power_manager_get_state(void)
{
    return s_state;
}
```

- [ ] **Step 3 : Écrire le CMakeLists du composant de test**

Create `test_app/components/test_power_manager/CMakeLists.txt` :

```cmake
# Composant de test pour main/power_manager.c
#
# power_manager.c vit dans main/, que test_app n'inclut pas. Mais
# power_manager.c est dependance-pure : on peut le compiler ici
# directement. Ce composant est auto-decouvert par ESP-IDF car il est
# dans test_app/components/.

idf_component_register(
    SRCS "test_power_manager.c"
         "../../../main/power_manager.c"
    INCLUDE_DIRS "../../../main"
    REQUIRES unity device_hal
    WHOLE_ARCHIVE
)
```

- [ ] **Step 4 : Écrire le test 1 — sur USB, jamais d'ÉCO**

Create `test_app/components/test_power_manager/test_power_manager.c` :

```c
/**
 * @file test_power_manager.c
 * @brief Tests de la machine d'energie power_manager.
 *
 * power_manager.c etant dependance-pure, tous les tests injectent des
 * fausses dependances : horloge simulee, source d'alim forcee, et des
 * mouchards qui enregistrent les appels backlight / esp_pm.
 */

#include "unity.h"
#include "power_manager.h"

/* --- Fausses dependances injectees --- */

static uint64_t           s_fake_now_ms;
static hal_power_source_t s_fake_source;
static uint8_t            s_last_backlight;
static int                s_backlight_calls;
static power_state_t      s_last_pm_state;
static int                s_pm_calls;

static uint64_t           fake_get_time_ms(void)        { return s_fake_now_ms; }
static hal_power_source_t fake_get_power_source(void)   { return s_fake_source; }
static void               fake_set_backlight(uint8_t p) { s_last_backlight = p; s_backlight_calls++; }
static void               fake_apply_pm(power_state_t s){ s_last_pm_state = s; s_pm_calls++; }
static void               fake_lock(void)               { /* mono-thread : no-op */ }
static void               fake_unlock(void)             { /* mono-thread : no-op */ }

/* Reinitialise les mouchards et initialise power_manager avec une
 * source d'alim donnee et un timeout court (5 s) pour des tests rapides. */
static void setup(hal_power_source_t source)
{
    s_fake_now_ms     = 1000;   /* depart non nul */
    s_fake_source     = source;
    s_last_backlight  = 255;
    s_backlight_calls = 0;
    s_last_pm_state   = (power_state_t)-1;
    s_pm_calls        = 0;

    power_manager_config_t cfg = {
        .get_time_ms      = fake_get_time_ms,
        .get_power_source = fake_get_power_source,
        .set_backlight    = fake_set_backlight,
        .apply_pm_config  = fake_apply_pm,
        .lock             = fake_lock,
        .unlock           = fake_unlock,
        .eco_timeout_ms   = 5000u,   /* 5 s pour les tests */
    };
    power_manager_init(&cfg);
}

TEST_CASE("power_usb_never_enters_eco", "[power_manager]")
{
    setup(POWER_SOURCE_USB);

    /* 10 minutes simulees sans aucune activite. */
    for (int i = 0; i < 600; i++) {
        s_fake_now_ms += 1000;
        power_manager_tick();
    }

    TEST_ASSERT_EQUAL(POWER_STATE_ACTIF, power_manager_get_state());
    /* Aucun passage en ECO => aucun appel backlight/pm declenche par tick. */
    TEST_ASSERT_EQUAL(0, s_backlight_calls);
    TEST_ASSERT_EQUAL(0, s_pm_calls);
}
```

- [ ] **Step 5 : Vérifier que le build test_app passe et que le test compile**

Run :
```bash
source ~/.espressif/v5.4.3/esp-idf/export.sh >/dev/null 2>&1 && idf.py -C test_app -B test_app/build-s3 build
```
Expected : `Project build complete`. Le composant `test_power_manager` est compilé, `power_manager.c` est lié.

- [ ] **Step 6 : Commit**

```bash
git add main/power_manager.h main/power_manager.c \
        test_app/components/test_power_manager/
git commit -m "feat(power): module power_manager + machine d'etats + test USB"
```

---

## Task 3 : Test 2 — sur batterie, passage en ÉCO après timeout

Vérifie qu'en `POWER_SOURCE_BATTERY`, après le timeout d'inactivité, la machine passe en ÉCO : backlight à 0 et config esp_pm ÉCO appliquée. La logique est déjà dans `power_manager.c` (Task 2) — ce test la valide.

**Files:**
- Modify: `test_app/components/test_power_manager/test_power_manager.c`

- [ ] **Step 1 : Ajouter le test 2**

Modify `test_app/components/test_power_manager/test_power_manager.c` — ajouter à la fin du fichier :

```c
TEST_CASE("power_battery_enters_eco_after_timeout", "[power_manager]")
{
    setup(POWER_SOURCE_BATTERY);

    /* 4 s d'inactivite : pas encore le timeout (5 s). */
    s_fake_now_ms += 4000;
    power_manager_tick();
    TEST_ASSERT_EQUAL(POWER_STATE_ACTIF, power_manager_get_state());

    /* 2 s de plus => 6 s >= 5 s => passage en ECO. */
    s_fake_now_ms += 2000;
    power_manager_tick();
    TEST_ASSERT_EQUAL(POWER_STATE_ECO, power_manager_get_state());

    /* Effets de bord du passage en ECO. */
    TEST_ASSERT_EQUAL(0, s_last_backlight);
    TEST_ASSERT_EQUAL(POWER_STATE_ECO, s_last_pm_state);
    TEST_ASSERT_EQUAL(1, s_backlight_calls);
    TEST_ASSERT_EQUAL(1, s_pm_calls);

    /* Idempotence : un tick supplementaire ne re-declenche rien. */
    s_fake_now_ms += 1000;
    power_manager_tick();
    TEST_ASSERT_EQUAL(1, s_backlight_calls);
    TEST_ASSERT_EQUAL(1, s_pm_calls);
}
```

- [ ] **Step 2 : Vérifier que le build test_app passe**

Run :
```bash
source ~/.espressif/v5.4.3/esp-idf/export.sh >/dev/null 2>&1 && idf.py -C test_app -B test_app/build-s3 build
```
Expected : `Project build complete`.

- [ ] **Step 3 : Commit**

```bash
git add test_app/components/test_power_manager/test_power_manager.c
git commit -m "test(power): passage en ECO sur batterie apres timeout"
```

---

## Task 4 : Test 3 — l'activité ramène en ACTIF

Vérifie qu'en ÉCO, `power_manager_notify_activity()` repasse immédiatement en ACTIF avec backlight à 100 et config esp_pm ACTIF.

**Files:**
- Modify: `test_app/components/test_power_manager/test_power_manager.c`

- [ ] **Step 1 : Ajouter le test 3**

Modify `test_app/components/test_power_manager/test_power_manager.c` — ajouter à la fin :

```c
TEST_CASE("power_activity_returns_to_actif", "[power_manager]")
{
    setup(POWER_SOURCE_BATTERY);

    /* Forcer le passage en ECO. */
    s_fake_now_ms += 6000;
    power_manager_tick();
    TEST_ASSERT_EQUAL(POWER_STATE_ECO, power_manager_get_state());

    /* Une interaction => retour immediat en ACTIF. */
    s_fake_now_ms += 500;
    power_manager_notify_activity();

    TEST_ASSERT_EQUAL(POWER_STATE_ACTIF, power_manager_get_state());
    TEST_ASSERT_EQUAL(100, s_last_backlight);
    TEST_ASSERT_EQUAL(POWER_STATE_ACTIF, s_last_pm_state);
    /* 2 appels backlight au total : 1 pour ECO, 1 pour le retour ACTIF. */
    TEST_ASSERT_EQUAL(2, s_backlight_calls);
    TEST_ASSERT_EQUAL(2, s_pm_calls);
}
```

- [ ] **Step 2 : Vérifier que le build test_app passe**

Run :
```bash
source ~/.espressif/v5.4.3/esp-idf/export.sh >/dev/null 2>&1 && idf.py -C test_app -B test_app/build-s3 build
```
Expected : `Project build complete`.

- [ ] **Step 3 : Commit**

```bash
git add test_app/components/test_power_manager/test_power_manager.c
git commit -m "test(power): retour immediat en ACTIF sur activite"
```

---

## Task 5 : Tests 4, 5, 6 — reset du timeout, source changée à chaud, UNKNOWN

Trois tests qui couvrent les cas restants de la spec §7.

**Files:**
- Modify: `test_app/components/test_power_manager/test_power_manager.c`

- [ ] **Step 1 : Ajouter les tests 4, 5, 6**

Modify `test_app/components/test_power_manager/test_power_manager.c` — ajouter à la fin :

```c
TEST_CASE("power_activity_resets_timeout", "[power_manager]")
{
    setup(POWER_SOURCE_BATTERY);

    /* Inactif pendant 4 s (timeout = 5 s). */
    s_fake_now_ms += 4000;
    power_manager_tick();
    TEST_ASSERT_EQUAL(POWER_STATE_ACTIF, power_manager_get_state());

    /* Activite a t=4 s : reset de l'horodatage. */
    power_manager_notify_activity();
    TEST_ASSERT_EQUAL(POWER_STATE_ACTIF, power_manager_get_state());

    /* 4 s de plus (t=8 s absolu, mais seulement 4 s depuis l'activite). */
    s_fake_now_ms += 4000;
    power_manager_tick();
    TEST_ASSERT_EQUAL(POWER_STATE_ACTIF, power_manager_get_state());

    /* 2 s de plus => 6 s depuis l'activite => ECO. */
    s_fake_now_ms += 2000;
    power_manager_tick();
    TEST_ASSERT_EQUAL(POWER_STATE_ECO, power_manager_get_state());
}

TEST_CASE("power_source_change_live_returns_to_actif", "[power_manager]")
{
    setup(POWER_SOURCE_BATTERY);

    /* Passer en ECO sur batterie. */
    s_fake_now_ms += 6000;
    power_manager_tick();
    TEST_ASSERT_EQUAL(POWER_STATE_ECO, power_manager_get_state());

    /* L'USB est branche : la source devient USB. */
    s_fake_source = POWER_SOURCE_USB;
    s_fake_now_ms += 1000;
    power_manager_tick();

    /* Le prochain tick force le retour en ACTIF. */
    TEST_ASSERT_EQUAL(POWER_STATE_ACTIF, power_manager_get_state());
    TEST_ASSERT_EQUAL(100, s_last_backlight);
    TEST_ASSERT_EQUAL(POWER_STATE_ACTIF, s_last_pm_state);
}

TEST_CASE("power_unknown_treated_as_battery", "[power_manager]")
{
    setup(POWER_SOURCE_UNKNOWN);

    /* UNKNOWN doit se comporter comme BATTERY : ECO apres timeout. */
    s_fake_now_ms += 6000;
    power_manager_tick();
    TEST_ASSERT_EQUAL(POWER_STATE_ECO, power_manager_get_state());
}
```

- [ ] **Step 2 : Vérifier que le build test_app passe**

Run :
```bash
source ~/.espressif/v5.4.3/esp-idf/export.sh >/dev/null 2>&1 && idf.py -C test_app -B test_app/build-s3 build
```
Expected : `Project build complete`.

- [ ] **Step 3 : Commit**

```bash
git add test_app/components/test_power_manager/test_power_manager.c
git commit -m "test(power): reset timeout, source a chaud, source UNKNOWN"
```

---

## Task 6 : `power_manager_stub.c` (CYD) + sélection CMake

Crée l'impl no-op pour le CYD et branche la sélection par cible dans `main/CMakeLists.txt`. Après cette task, le firmware compile pour les deux cibles (mais `power_manager` n'est pas encore appelé par `app_main` — c'est la Task 8).

**Files:**
- Create: `main/power_manager_stub.c`
- Modify: `main/CMakeLists.txt`

- [ ] **Step 1 : Écrire le stub no-op**

Create `main/power_manager_stub.c` :

```c
/**
 * @file power_manager_stub.c
 * @brief Implementation no-op de power_manager pour le CYD (ESP32).
 *
 * Le CYD est le noeud maitre, typiquement sur USB, qui doit rester
 * pleinement reactif. Il n'a pas de gestion d'energie : toutes les
 * fonctions sont des no-ops et get_state renvoie toujours ACTIF.
 *
 * Compile uniquement sur ESP32 (cf. main/CMakeLists.txt). Sur le S3
 * c'est power_manager.c (impl reelle) qui est lie.
 */

#include "power_manager.h"

void power_manager_init(const power_manager_config_t *cfg)
{
    (void)cfg;
}

void power_manager_notify_activity(void)
{
}

void power_manager_tick(void)
{
}

power_state_t power_manager_get_state(void)
{
    return POWER_STATE_ACTIF;
}
```

- [ ] **Step 2 : Brancher la sélection par cible dans main/CMakeLists.txt**

Modify `main/CMakeLists.txt`. Juste après le bloc de sélection `transport_lora` (`if(CONFIG_IDF_TARGET_ESP32) ... transport_lora.c ... else() ... transport_lora_stub.c ... endif()`), ajouter un bloc symétrique :

```cmake
# Gestion de l'energie (feature 13) : impl reelle sur ESP32-S3, stub
# no-op sur le CYD (ESP32). Le CYD reste toujours pleinement reactif.
if(CONFIG_IDF_TARGET_ESP32S3)
    list(APPEND meshpay_main_srcs "power_manager.c")
else()
    list(APPEND meshpay_main_srcs "power_manager_stub.c")
endif()
```

- [ ] **Step 3 : Vérifier le build des deux cibles**

Run (ESP32-S3) :
```bash
source ~/.espressif/v5.4.3/esp-idf/export.sh >/dev/null 2>&1 && rm -f sdkconfig && idf.py --no-hints -B build-s3 build
```
Expected : `Project build complete`.

Run (ESP32 / CYD) :
```bash
source ~/.espressif/v5.4.3/esp-idf/export.sh >/dev/null 2>&1 && rm -f sdkconfig && idf.py --no-hints -B build-esp32 -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.esp32" build
```
Expected : `Project build complete`.

- [ ] **Step 4 : Commit**

```bash
git add main/power_manager_stub.c main/CMakeLists.txt
git commit -m "feat(power): stub power_manager no-op pour le CYD + selection CMake"
```

---

## Task 7 : Hook `transport_lora_set_sync_interval` (inerte)

Ajoute le point d'extension qui permettra plus tard de ralentir la sync LoRa en ÉCO. Inerte aujourd'hui : `lora_sync_task` ne tourne pas sur le S3 (stub), et sur le CYD le `power_manager` est le stub donc personne ne l'appelle. Mais l'API doit exister pour que `power_manager` (impl réelle) puisse la câbler en Task 8.

**Files:**
- Modify: `main/transport/transport_lora.h`
- Modify: `main/transport/transport_lora.c`
- Modify: `main/transport/transport_lora_stub.c`

- [ ] **Step 1 : Déclarer la fonction dans le header**

Modify `main/transport/transport_lora.h`. Après la déclaration de `transport_lora_pump(void);` (vers la ligne 95), ajouter :

```c
/**
 * @brief Regle l'intervalle de sync LoRa (ms).
 *
 * Permet au power_manager de ralentir la sync en mode ECO. Sur le stub
 * (cibles sans LoRa) : no-op. Sur l'impl reelle : memorise la valeur ;
 * elle sera prise en compte au prochain cycle de lora_sync_task.
 *
 * NB : aujourd'hui inerte sur ESP32-S3 (lora_sync_task n'y tourne pas
 * encore) et non appele sur le CYD (power_manager y est le stub). Le
 * hook est pose pour le jour ou le S3 aura du LoRa.
 *
 * @param interval_ms Nouvel intervalle de sync en millisecondes.
 */
void transport_lora_set_sync_interval(uint32_t interval_ms);
```

- [ ] **Step 2 : Implémenter dans transport_lora.c (impl réelle)**

Modify `main/transport/transport_lora.c`. La struct `s_lora_cfg` (de type `lora_sync_config_t`) existe déjà comme variable statique du module et contient le champ `sync_interval_ms`. Ajouter la fonction à la fin du fichier :

```c
void transport_lora_set_sync_interval(uint32_t interval_ms)
{
    /* Memorise la nouvelle valeur dans la config de lora_sync_task.
     * lora_sync_task relit sync_interval_ms a chaque cycle (cf.
     * lora_sync.c : "Dormir sync_interval_ms"), donc la prise en
     * compte est effective au cycle suivant. */
    s_lora_cfg.sync_interval_ms = interval_ms;
}
```

> **Note :** vérifier que `s_lora_cfg` est bien accessible (variable statique au niveau fichier dans `transport_lora.c`). C'est le cas d'après l'inspection du Lot D.3 — elle est déclarée `static lora_sync_config_t s_lora_cfg;`. Si l'accès pose problème, la fonction peut rester un no-op documenté pour la Phase 1, le câblage réel étant fait quand le S3 aura LoRa.

- [ ] **Step 3 : Implémenter le no-op dans transport_lora_stub.c**

Modify `main/transport/transport_lora_stub.c`. Ajouter à la fin :

```c
void transport_lora_set_sync_interval(uint32_t interval_ms)
{
    (void)interval_ms;
}
```

- [ ] **Step 4 : Vérifier le build des deux cibles**

Run (ESP32-S3) :
```bash
source ~/.espressif/v5.4.3/esp-idf/export.sh >/dev/null 2>&1 && rm -f sdkconfig && idf.py --no-hints -B build-s3 build
```
Expected : `Project build complete`.

Run (ESP32 / CYD) :
```bash
source ~/.espressif/v5.4.3/esp-idf/export.sh >/dev/null 2>&1 && rm -f sdkconfig && idf.py --no-hints -B build-esp32 -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.esp32" build
```
Expected : `Project build complete`.

- [ ] **Step 5 : Commit**

```bash
git add main/transport/transport_lora.h main/transport/transport_lora.c \
        main/transport/transport_lora_stub.c
git commit -m "feat(power): hook transport_lora_set_sync_interval (inerte Phase 1)"
```

---

## Task 8 : Intégration — `app_main`, `core_task`, `ui_task`, sdkconfig

Câble tout : init `hal_power` + `power_manager` dans `app_main`, signaux d'activité depuis `core_task` et `ui_task`, adaptateur `esp_pm`, mutex dédié, et `CONFIG_PM_ENABLE` dans le sdkconfig S3. Après cette task la feature est complète et compile pour les deux cibles.

**Files:**
- Modify: `main/main.c`
- Modify: `main/core_task.c`
- Modify: `components/ui/include/ui/ui_state.h`
- Modify: `components/ui/src/ui_task.c`
- Modify: `sdkconfig.defaults.esp32s3`

- [ ] **Step 1 : Ajouter `CONFIG_PM_ENABLE` au sdkconfig S3**

Modify `sdkconfig.defaults.esp32s3`. Ajouter à la fin du fichier :

```
# --- Gestion de l'energie (feature 13) ---
# Active esp_pm pour le CPU frequency scaling en mode ECO.
# Phase 1 : frequency scaling seul (pas de light sleep). Le light sleep
# (CONFIG_FREERTOS_USE_TICKLESS_IDLE) viendra en Phase 2.
CONFIG_PM_ENABLE=y
```

- [ ] **Step 2 : Ajouter le callback d'activité dans le contexte UI**

Modify `components/ui/include/ui/ui_state.h`. Dans la struct `ui_ctx_s`, après le champ `get_owner_balance` (callback existant, vers la fin de la struct), ajouter :

```c
    /** Signale une interaction utilisateur au gestionnaire d'energie.
     *  Appele par ui_task sur chaque touch detecte. NULL autorise
     *  (le gestionnaire d'energie est optionnel selon la cible). */
    void (*notify_activity)(void);
```

- [ ] **Step 3 : Appeler `notify_activity` sur touch dans ui_task**

Modify `components/ui/src/ui_task.c`. Dans `lvgl_touch_read_cb` (vers la ligne 80), à l'intérieur du `if (err == HAL_OK && pt.pressed)`, ajouter l'appel au callback. Le bloc devient :

```c
static void lvgl_touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    ui_ctx_t *ctx = lv_indev_get_user_data(indev);

    hal_touch_point_t pt;
    hal_err_t err = ctx->display->touch_read(&pt, ctx->display->ctx);

    if (err == HAL_OK && pt.pressed) {
        data->point.x = pt.x;
        data->point.y = pt.y;
        data->state = LV_INDEV_STATE_PRESSED;
        /* Signaler l'interaction au gestionnaire d'energie (feature 13). */
        if (ctx->notify_activity != NULL) {
            ctx->notify_activity();
        }
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}
```

- [ ] **Step 4 : Signaux d'activité + tick dans core_task**

Modify `main/core_task.c`. Deux changements dans la fonction `core_task` :

(a) Ajouter l'include en tête de fichier, après les includes existants :
```c
#include "power_manager.h"
```

(b) Dans la boucle `for (;;)`, signaler l'activité sur événement reçu et sur commande UI, et appeler `tick()` à chaque tour. Le `notify_activity()` doit être appelé **hors** du `s_state_mutex` (power_manager a son propre verrou). Repérer la structure existante :

- après `xQueueReceive(s_evt_queue, &evt, ...)` qui renvoie `got` ;
- le bloc `if (got == pdTRUE) { switch (evt.type) ... }` est sous `s_state_mutex` ;
- le drainage des `ui_cmd_t` est sous `s_state_mutex` ;
- `xSemaphoreGive(s_state_mutex)` puis `transport_lora_pump()` hors mutex.

Ajouter, **juste après `xSemaphoreGive(s_state_mutex);` et avant `transport_lora_pump();`** :

```c
        /* Gestion de l'energie (feature 13) — hors mutex applicatif :
         * power_manager a son propre verrou. */
        if (got == pdTRUE) {
            /* Un evenement reseau est une interaction reseau. */
            power_manager_notify_activity();
        }
        power_manager_tick();
```

> Les commandes UI : `notify_activity` est déjà déclenché côté `ui_task` (touch). Le drainage `ui_cmd_t` dans `core_task` correspond à une action utilisateur déjà signalée par le touch qui l'a produite — pas besoin de doubler. On garde donc uniquement le signal « événement réseau » + le `tick()` ici.

- [ ] **Step 5 : Init hal_power + power_manager + adaptateurs dans app_main**

Modify `main/main.c`.

(a) Ajouter les includes, après `#include "core_task.h"` / `#include "ui_dispatch.h"` (bloc d'includes du Lot D.6) :
```c
#include "power_manager.h"
#include "hal/hal_power.h"
#include "esp_pm.h"
```

(b) Ajouter, au niveau fichier (après le bloc des `extern` factories HAL, avant `app_main`), les variables et adaptateurs nécessaires :
```c
/* ================================================================
 * Gestion de l'energie (feature 13) — adaptateurs pour power_manager
 * ================================================================ */

/* Frequences CPU (MHz) pilotees par esp_pm selon l'etat d'energie.
 * Definies ici (et non dans power_manager.c) car power_manager.c est
 * dependance-pure : il ne connait pas esp_pm. */
#define POWER_MAX_FREQ_MHZ        240
#define POWER_ACTIF_MIN_FREQ_MHZ  240   /* ACTIF : pas de scaling */
#define POWER_ECO_MIN_FREQ_MHZ     80   /* ECO   : scaling autorise */

/* HAL source d'alimentation (stub : toujours USB tant que le hardware
 * batterie n'existe pas). */
static hal_power_t s_power_hal;

/* Mutex dedie au power_manager : protege sa machine d'etats entre
 * core_task (tick) et ui_task (notify_activity). Dedie pour ne pas
 * interferer avec s_state_mutex. */
static SemaphoreHandle_t s_power_mutex;

/* Adaptateur get_power_source : power_manager attend une fonction sans
 * argument, hal_power expose une vtable avec ctx. */
static hal_power_source_t power_get_source_adapter(void)
{
    return s_power_hal.get_source(s_power_hal.ctx);
}

/* Adaptateur set_backlight : delegue au HAL display. */
static void power_set_backlight_adapter(uint8_t pct)
{
    if (s_display.set_backlight != NULL) {
        s_display.set_backlight(pct, s_display.ctx);
    }
}

/* Adaptateur apply_pm_config : configure esp_pm selon l'etat.
 * Phase 1 : frequency scaling seul, light_sleep_enable reste false. */
static void power_apply_pm_config(power_state_t state)
{
    esp_pm_config_t pm = {
        .max_freq_mhz       = POWER_MAX_FREQ_MHZ,
        .min_freq_mhz       = (state == POWER_STATE_ECO)
                                  ? POWER_ECO_MIN_FREQ_MHZ
                                  : POWER_ACTIF_MIN_FREQ_MHZ,
        .light_sleep_enable = false,   /* Phase 1 : pas de light sleep */
    };
    esp_err_t err = esp_pm_configure(&pm);
    if (err != ESP_OK) {
        /* Sur le CYD, CONFIG_PM_ENABLE n'est pas active : esp_pm_configure
         * renvoie ESP_ERR_NOT_SUPPORTED. C'est attendu et sans consequence
         * (le power_manager du CYD est le stub, il n'appelle jamais ceci ;
         * seul l'appel de boot ci-dessous le declenche). */
        ESP_LOGD(TAG, "esp_pm_configure: 0x%x (attendu si PM desactive)", err);
    }
}

/* Adaptateurs lock/unlock : mutex dedie au power_manager. */
static void power_lock(void)   { xSemaphoreTake(s_power_mutex, portMAX_DELAY); }
static void power_unlock(void) { xSemaphoreGive(s_power_mutex); }
```

(c) Dans `app_main`, **après la création de `s_state_mutex`** (section « ---- 12. Queues, mutex et taches ---- ») et **avant `xTaskCreate(core_task, ...)`**, ajouter l'init de l'énergie :
```c
    /* ---- Gestion de l'energie (feature 13) ---- */
    /* Config esp_pm de boot : etat ACTIF (pas de scaling). */
    power_apply_pm_config(POWER_STATE_ACTIF);

    s_power_mutex = xSemaphoreCreateMutex();
    if (s_power_mutex == NULL) {
        ESP_LOGE(TAG, "Erreur creation s_power_mutex");
        return;
    }

    hal_power_stub_create(&s_power_hal);

    power_manager_config_t power_cfg = {
        .get_time_ms      = get_time_ms_wrapper,
        .get_power_source = power_get_source_adapter,
        .set_backlight    = power_set_backlight_adapter,
        .apply_pm_config  = power_apply_pm_config,
        .lock             = power_lock,
        .unlock           = power_unlock,
        .eco_timeout_ms   = POWER_ECO_TIMEOUT_MS,
    };
    power_manager_init(&power_cfg);
    ESP_LOGI(TAG, "Gestion energie initialisee (etat ACTIF, source=%s)",
             power_get_source_adapter() == POWER_SOURCE_USB ? "USB" : "batterie");
```

(d) Dans le remplissage de `s_ui_ctx` (section « ---- 13. UI task ---- », là où les autres champs `s_ui_ctx.xxx = ...` sont assignés), ajouter :
```c
    s_ui_ctx.notify_activity = power_manager_notify_activity;
```

> **Vérifications avant build :**
> - `get_time_ms_wrapper` est déclaré dans `time_glue.h` (déjà inclus par `main.c` via le bloc Lot D). Confirmer.
> - `s_display` est la variable globale du HAL display (dans `app_state`). Confirmer qu'elle est accessible depuis `main.c` (elle l'est : `main.c` inclut `app_state.h`).
> - `esp_pm.h` nécessite le composant `esp_pm`. Vérifier que `main/CMakeLists.txt` a `esp_pm` dans `REQUIRES` ; sinon l'ajouter.

- [ ] **Step 6 : Ajouter `esp_pm` aux REQUIRES de main si nécessaire**

Modify `main/CMakeLists.txt`. Dans le `idf_component_register`, vérifier la ligne `REQUIRES`. Si `esp_pm` n'y est pas, l'ajouter à la liste (à côté de `esp_timer`, `esp_wifi`, etc.) :

```cmake
    REQUIRES wallet dag transaction crypto
             comm_protocol espnow lora_sync
             currency time_manager device_hal
             nvs_flash esp_timer esp_wifi esp_pm
             ui
             debug_console
```

- [ ] **Step 7 : Build ESP32-S3 + corriger les erreurs éventuelles**

Run :
```bash
source ~/.espressif/v5.4.3/esp-idf/export.sh >/dev/null 2>&1 && rm -f sdkconfig && idf.py --no-hints -B build-s3 build
```
Expected : `Project build complete`. Vérifier dans la sortie qu'aucun warning `-Werror` n'apparaît sur les fichiers modifiés.

- [ ] **Step 8 : Build ESP32 (CYD)**

Run :
```bash
source ~/.espressif/v5.4.3/esp-idf/export.sh >/dev/null 2>&1 && rm -f sdkconfig && idf.py --no-hints -B build-esp32 -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.esp32" build
```
Expected : `Project build complete`.

- [ ] **Step 9 : Build de l'app de test**

Run :
```bash
source ~/.espressif/v5.4.3/esp-idf/export.sh >/dev/null 2>&1 && rm -f sdkconfig && idf.py -C test_app -B test_app/build-s3 build
```
Expected : `Project build complete`.

- [ ] **Step 10 : Commit**

```bash
git add main/main.c main/core_task.c main/CMakeLists.txt \
        components/ui/include/ui/ui_state.h components/ui/src/ui_task.c \
        sdkconfig.defaults.esp32s3
git commit -m "feat(power): integrer power_manager dans app_main, core_task et ui_task"
```

---

## Task 9 : Mise à jour de la documentation

Met à jour la doc Obsidian : le spec passe en « implémenté », et le journal des corrections reçoit une entrée.

**Files:**
- Modify: `/Users/misterniark/Documents/Obsidian/Misterniark/Projet/Mesh Pay/Doctech/13 - Gestion de l'énergie (design).md`
- Modify: `/Users/misterniark/Documents/Obsidian/Misterniark/Projet/Mesh Pay/Doctech/08 - Journal des corrections récentes.md`

- [ ] **Step 1 : Marquer le spec comme implémenté**

Modify le frontmatter de `13 - Gestion de l'énergie (design).md` : remplacer `status: design validé — à implémenter` par `status: Phase 1 implémentée 2026-05-14`.

- [ ] **Step 2 : Ajouter une entrée au journal des corrections**

Modify `08 - Journal des corrections récentes.md`. Ajouter une section avant la section `## Voir aussi` finale :

```markdown
---

# Feature 13 — Gestion de l'énergie Phase 1 (mai 2026)

**Contexte** : pose de l'infrastructure de gestion d'énergie sur le Waveshare ESP32-S3. Détail : [[13 - Gestion de l'énergie (design)]].

**Livré (Phase 1)** :
- HAL `hal_power` + impl stub (toujours USB) — `components/device_hal/`.
- Module `power_manager` : machine d'états ACTIF/ÉCO déclenchée par inactivité (timeout 120 s sur batterie, jamais sur USB). Dépendance-pure, 6 tests Unity natifs.
- État ÉCO Phase 1 : backlight éteint + CPU frequency scaling (`esp_pm`, sans light sleep).
- Pattern facade+stub : `power_manager.c` (ESP32-S3) vs `power_manager_stub.c` (CYD no-op).
- Hook `transport_lora_set_sync_interval()` posé (inerte tant que le S3 n'a pas LoRa).

**Inerte aujourd'hui** : le stub `hal_power` renvoie toujours USB → le device reste toujours ACTIF. L'infrastructure s'activera quand le vrai `hal_power` (lecture GPIO/ADC) sera câblé.

**Reporté** : light sleep (Phase 2), participation du S3 au DAG via LoRa (prérequis séparé).

**Builds** : ESP32 + ESP32-S3 + test_app OK.
```

- [ ] **Step 3 : (pas de commit git)**

Le dossier Obsidian Doctech n'est pas un repo git (pattern établi du projet). Les fichiers sont simplement sauvegardés. Aucune commande git.

---

## Validation finale (hors plan, sur hardware)

Après exécution complète du plan, l'utilisateur valide sur device :

1. **Flasher `test_app` sur le Waveshare S3** et lancer les 6 cas `[power_manager]` + les 2 cas `[hal_power]` via le menu Unity série. Tous doivent passer.
2. **Flasher le firmware S3** (`build-s3`) : vérifier au moniteur série le log `Gestion energie initialisee (etat ACTIF, source=USB)` et l'absence de régression (le device reste ACTIF, backlight allumé — normal, stub = USB).
3. **Flasher le firmware CYD** (`build-esp32`) : vérifier l'absence de régression (le `power_manager_stub` ne fait rien).

> La machine ÉCO elle-même ne pourra être validée sur device qu'avec un vrai `hal_power` câblé (hardware batterie) — d'ici là, les 6 tests Unity sont la garantie.
