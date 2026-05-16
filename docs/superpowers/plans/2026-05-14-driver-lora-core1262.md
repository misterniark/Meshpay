# Driver LoRa Core1262 (SX1262) — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Ajouter un backend HAL LoRa pour le module Waveshare Core1262 (SX1262 en SPI), sélectionnable par Kconfig, en gardant le driver Wio-E5 disponible comme alternative.

**Architecture:** Le HAL `hal_lora_t` (vtable de 5 fonctions) est déjà la frontière d'abstraction. On ajoute un 2ᵉ backend (`hal_lora_core1262.c`) qui pilote le SX1262 via le pilote C officiel Semtech vendoré + une glue plateforme ESP-IDF (SPI/GPIO). Une factory unifiée `hal_lora_create_default()` choisit le driver selon une option Kconfig. Le LoRa devient obligatoire sur toutes les cibles (le stub no-op disparaît).

**Tech Stack:** ESP-IDF (CMake), C, FreeRTOS, `driver/spi_master.h` + `driver/gpio.h`, pilote Semtech `sx126x_driver` (BSD-3), Unity pour les tests.

**Référence design:** `Doctech/14 - Driver LoRa Core1262 (design).md` (vault Obsidian).

---

## Contexte pour l'implémenteur

Tu ne connais pas ce codebase. Points clés :

- **HAL LoRa** : `components/device_hal/include/hal/hal_lora.h` définit `hal_lora_t` (vtable : `init`, `send`, `set_rx_callback`, `start_rx`, `sleep` + `void *ctx`), `hal_lora_config_t` (freq, SF, BW, CR, puissance), et `HAL_LORA_MAX_PACKET_SIZE` (255).
- **Codes d'erreur** : `hal_err_t` dans `hal_types.h` — `HAL_OK=0`, `HAL_FAIL=-1`, `HAL_ERR_NO_MEM=-3`, `HAL_ERR_INVALID=-4`, `HAL_ERR_TIMEOUT=-5`, `HAL_ERR_BUSY=-6`, `HAL_ERR_IO=-7`.
- **Backend existant** : `components/device_hal/src/esp32/hal_lora_wio_e5.c` — driver UART/AT, factory `hal_lora_wio_e5_create(hal_lora_t*, int uart_num, int tx_pin, int rx_pin)`. Sert de modèle de structure (contexte statique, tâche RX FreeRTOS).
- **Façade applicative** : `main/transport/transport_lora.c` instancie le HAL et lance `lora_sync_task`. C'est le seul appelant de la factory. Aujourd'hui il appelle `hal_lora_wio_e5_create` en dur (ligne 30 `extern`, ligne 145 appel) et définit les pins en `#define` (lignes 39-42).
- **Sélection actuelle par cible** : `device_hal/CMakeLists.txt` ne compile `hal_lora_wio_e5.c` que `if(CONFIG_IDF_TARGET_ESP32)`. `main/CMakeLists.txt` choisit `transport_lora.c` (réel) vs `transport_lora_stub.c` (no-op) selon la même condition. **On remplace ce gating par cible par un gating Kconfig.**
- **Tests** : projet ESP-IDF séparé `test_app/` ; `idf.py -C test_app build`. Les composants ajoutent leurs fichiers de test quand `PROJECT_NAME == "meshpay_test_app"`. Pattern Unity : `TEST_CASE("nom", "[tag]")`, `setUp`/`tearDown` en `__attribute__((weak))`. Le `test_app` réutilise `../components/device_hal` via `EXTRA_COMPONENT_DIRS` mais **n'inclut pas `main/`** → supprimer `transport_lora_stub.c` est sûr.
- **Config radio** : `components/comm/lora_sync/src/lora_sync.c` (~ligne 678) construit `hal_lora_config_t` (868100000 Hz, SF9, BW=0/125kHz, CR=1/4-5, 14 dBm) et appelle `config->lora->init(...)`. **Ne pas toucher** — le backend Core1262 consomme ce `hal_lora_config_t`.
- **Câblage Core1262 ↔ ESP32-S3** (vérifié sur schémas Waveshare, MISO révisé mai 2026) : SPI3_HOST. SCK=IO1, MOSI=IO2, **MISO=IO10** *(déplacé de IO3 — voir note ci-dessous)*, NSS=IO4, RESET=IO5, BUSY=IO6, DIO1=IO7, RXEN=IO8, TXEN=IO9. DIO2 non connecté. DIO3 = TCXO interne (géré par firmware). Alim 3,3 V.

  > **Note MISO** : initialement câblé sur IO3, déplacé sur IO10 en mai 2026. IO3 est un *strapping pin* JTAG sur ESP32-S3 (source du signal JTAG) ; avec la console USB-Serial-JTAG active (`CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y`), l'état flottant/conducteur du SX1262 MISO au boot rendait l'init Core1262 instable et, combiné au bug F-LT-001 (`s_lora_hal.send` NULL appelé sans garde), crashait le firmware au premier paiement reçu. Recâblage physique du fil MISO sur le header P1 + override `CONFIG_MESHPAY_LORA_C1262_PIN_MISO=10` dans `sdkconfig.defaults.esp32s3`.

**Conventions du projet :** commentaires en français, détaillés. Build CMake. Tests unitaires pour les nouvelles fonctions pures.

**Note honnête sur les Tasks 6-8 (radio) :** le code de bring-up SX1262 (séquence registre, config PA, TCXO) est écrit ici d'après la séquence Semtech documentée. Certaines valeurs (tension TCXO, config PA fine) peuvent demander un ajustement à la **Task 9 (smoke test matériel)** — c'est attendu, ce n'est pas un défaut du plan. Le code compile et est structurellement correct ; la validation RF se fait sur hardware.

---

## File Structure

### Créés

| Fichier | Responsabilité |
| ------- | -------------- |
| `components/device_hal/Kconfig` | Choix du driver LoRa + GPIO Core1262 + pins Wio-E5 |
| `components/device_hal/include/hal/hal_lora_factory.h` | API publique `hal_lora_create_default()` |
| `components/device_hal/src/hal_lora_factory.c` | Sélection du backend selon Kconfig — unique point de variation matérielle |
| `components/device_hal/src/esp32/sx126x/` | Pilote Semtech `sx126x_driver` vendoré tel quel |
| `components/device_hal/src/esp32/sx126x_hal_context.h` | Struct `core1262_hw_t` partagée entre la glue et le backend |
| `components/device_hal/src/esp32/sx126x_hal.c` | Glue plateforme : implémente `sx126x_hal_write/read/reset/wakeup` (SPI/GPIO ESP-IDF) |
| `components/device_hal/src/esp32/core1262_params.h` | Type `core1262_radio_params_t` + déclaration du mapper |
| `components/device_hal/src/esp32/core1262_params.c` | Fonction pure `core1262_map_config()` : `hal_lora_config_t` → paramètres SX1262 |
| `components/device_hal/src/esp32/hal_lora_core1262.h` | Factory `hal_lora_core1262_create()` |
| `components/device_hal/src/esp32/hal_lora_core1262.c` | Backend HAL : vtable `hal_lora_t`, init radio, send bloquant, tâche RX sur IRQ DIO1 |
| `components/device_hal/test/test_core1262_params.c` | Tests unitaires Unity du mapper |

### Modifiés

| Fichier | Changement |
| ------- | ---------- |
| `components/device_hal/CMakeLists.txt` | Driver LoRa piloté par Kconfig (hors blocs `CONFIG_IDF_TARGET_*`) + factory + sx126x |
| `main/transport/transport_lora.c` | Appel à `hal_lora_create_default()`, suppression des `#define` pins |
| `main/CMakeLists.txt` | `transport_lora.c` toujours compilé, suppression du gating stub |
| `sdkconfig.defaults.esp32` | `CONFIG_MESHPAY_LORA_DRIVER_WIO_E5=y` |
| `sdkconfig.defaults.esp32s3` | `CONFIG_MESHPAY_LORA_DRIVER_CORE1262=y` |

### Supprimés

| Fichier | Raison |
| ------- | ------ |
| `main/transport/transport_lora_stub.c` | LoRa n'est plus optionnel |

---

## Task 1: Kconfig du composant device_hal

**Files:**
- Create: `components/device_hal/Kconfig`

Le `choice` a pour défaut `WIO_E5` : pendant toute la phase de refactoring (Tasks 1-7), les builds restent sur le driver Wio-E5 fonctionnel. Le défaut basculera à `CORE1262` en Task 8.

- [ ] **Step 1: Créer le fichier Kconfig**

Create `components/device_hal/Kconfig`:

```
menu "Mesh Pay - LoRa"

choice MESHPAY_LORA_DRIVER
    prompt "Driver radio LoRa"
    default MESHPAY_LORA_DRIVER_WIO_E5
    help
        Choix du module radio LoRa. Le LoRa est obligatoire sur tous les
        devices ; cette option choisit uniquement quel driver materiel est
        compile. Le code applicatif passe par hal_lora_create_default().

    config MESHPAY_LORA_DRIVER_WIO_E5
        bool "Grove Wio-E5 (UART / commandes AT)"

    config MESHPAY_LORA_DRIVER_CORE1262
        bool "Waveshare Core1262 (SX1262, SPI)"
endchoice

# --- Pins Core1262 (SX1262 SPI) — utilises si CORE1262 ---
# Defauts verifies sur les schemas officiels Waveshare : header
# d'extension P1 de l'ESP32-S3-Touch-LCD-1.47, GPIO IO1-IO11 libres.
# SPI3_HOST = 2 dans l'enumeration spi_host_device_t d'ESP-IDF.
config MESHPAY_LORA_C1262_SPI_HOST
    int "Core1262 : hote SPI"
    depends on MESHPAY_LORA_DRIVER_CORE1262
    default 2

config MESHPAY_LORA_C1262_PIN_SCK
    int "Core1262 : GPIO SCK"
    depends on MESHPAY_LORA_DRIVER_CORE1262
    default 1

config MESHPAY_LORA_C1262_PIN_MOSI
    int "Core1262 : GPIO MOSI"
    depends on MESHPAY_LORA_DRIVER_CORE1262
    default 2

config MESHPAY_LORA_C1262_PIN_MISO
    int "Core1262 : GPIO MISO"
    depends on MESHPAY_LORA_DRIVER_CORE1262
    default 10  # ex-3, deplace mai 2026 (strap pin JTAG sur ESP32-S3)

config MESHPAY_LORA_C1262_PIN_NSS
    int "Core1262 : GPIO NSS (chip-select)"
    depends on MESHPAY_LORA_DRIVER_CORE1262
    default 4

config MESHPAY_LORA_C1262_PIN_RESET
    int "Core1262 : GPIO RESET"
    depends on MESHPAY_LORA_DRIVER_CORE1262
    default 5

config MESHPAY_LORA_C1262_PIN_BUSY
    int "Core1262 : GPIO BUSY"
    depends on MESHPAY_LORA_DRIVER_CORE1262
    default 6

config MESHPAY_LORA_C1262_PIN_DIO1
    int "Core1262 : GPIO DIO1 (IRQ)"
    depends on MESHPAY_LORA_DRIVER_CORE1262
    default 7

config MESHPAY_LORA_C1262_PIN_RXEN
    int "Core1262 : GPIO RXEN (switch RF RX)"
    depends on MESHPAY_LORA_DRIVER_CORE1262
    default 8

config MESHPAY_LORA_C1262_PIN_TXEN
    int "Core1262 : GPIO TXEN (switch RF TX)"
    depends on MESHPAY_LORA_DRIVER_CORE1262
    default 9

# --- Pins Wio-E5 (UART/AT) — utilises si WIO_E5 ---
# Anciennement des #define dans main/transport/transport_lora.c.
config MESHPAY_LORA_WIOE5_UART_NUM
    int "Wio-E5 : numero de port UART"
    depends on MESHPAY_LORA_DRIVER_WIO_E5
    default 2

config MESHPAY_LORA_WIOE5_PIN_TX
    int "Wio-E5 : GPIO TX"
    depends on MESHPAY_LORA_DRIVER_WIO_E5
    default 17

config MESHPAY_LORA_WIOE5_PIN_RX
    int "Wio-E5 : GPIO RX"
    depends on MESHPAY_LORA_DRIVER_WIO_E5
    default 16

endmenu
```

- [ ] **Step 2: Vérifier que le Kconfig est pris en compte et que le build reste vert**

Run: `idf.py set-target esp32 && idf.py build`
Expected: build OK. `idf.py menuconfig` afficherait désormais le menu "Mesh Pay - LoRa" avec le choix de driver (défaut Wio-E5). Aucune option n'est encore consommée — c'est normal.

- [ ] **Step 3: Commit**

```bash
git add components/device_hal/Kconfig
git commit -m "feat(device_hal): Kconfig de selection du driver LoRa (Wio-E5 / Core1262)

Ajoute l'option CONFIG_MESHPAY_LORA_DRIVER et les GPIO configurables
des deux drivers. Defaut Wio-E5 : aucun changement de comportement.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>"
```

---

## Task 2: Factory unifiée `hal_lora_create_default()`

**Files:**
- Create: `components/device_hal/include/hal/hal_lora_factory.h`
- Create: `components/device_hal/src/hal_lora_factory.c`
- Modify: `components/device_hal/CMakeLists.txt`

La factory fait le `#if` Kconfig et lit les pins. La branche `CORE1262` référence `hal_lora_core1262_create` qui n'existe pas encore — mais comme le défaut Kconfig est `WIO_E5`, cette branche est exclue à la compilation. Elle deviendra active en Task 7/8.

- [ ] **Step 1: Créer l'en-tête de la factory**

Create `components/device_hal/include/hal/hal_lora_factory.h`:

```c
/**
 * @file hal_lora_factory.h
 * @brief Factory unifiee de creation du HAL LoRa.
 *
 * Point d'entree unique pour le code applicatif : il appelle
 * hal_lora_create_default() sans connaitre le module radio concret.
 * Le choix Wio-E5 / Core1262 se fait par l'option Kconfig
 * CONFIG_MESHPAY_LORA_DRIVER ; les GPIO/UART sont lus depuis Kconfig.
 *
 * C'est l'unique fichier qui connait les details materiels de chaque
 * puce : ajouter un 3e driver demain ne touche que ce fichier.
 */

#ifndef HAL_LORA_FACTORY_H
#define HAL_LORA_FACTORY_H

#include "hal/hal_lora.h"

/**
 * Cree l'instance LoRa correspondant au driver selectionne en Kconfig.
 *
 * @param lora [out] Vtable a remplir
 * @return HAL_OK en cas de succes, HAL_ERR_INVALID si lora == NULL,
 *         ou le code d'erreur de la factory concrete.
 */
hal_err_t hal_lora_create_default(hal_lora_t *lora);

#endif /* HAL_LORA_FACTORY_H */
```

- [ ] **Step 2: Créer l'implémentation de la factory**

Create `components/device_hal/src/hal_lora_factory.c`:

```c
/**
 * @file hal_lora_factory.c
 * @brief Selection du backend HAL LoRa selon CONFIG_MESHPAY_LORA_DRIVER.
 *
 * Chaque branche #if lit les GPIO/UART depuis Kconfig et delegue a la
 * factory concrete du driver. Une seule branche est compilee.
 */

#include "hal/hal_lora_factory.h"
#include "sdkconfig.h"

#if defined(CONFIG_MESHPAY_LORA_DRIVER_WIO_E5)

/* Factory du driver Wio-E5 (UART/AT) — definie dans esp32/hal_lora_wio_e5.c */
extern hal_err_t hal_lora_wio_e5_create(hal_lora_t *lora, int uart_num,
                                        int tx_pin, int rx_pin);

hal_err_t hal_lora_create_default(hal_lora_t *lora)
{
    if (!lora) {
        return HAL_ERR_INVALID;
    }
    return hal_lora_wio_e5_create(lora,
                                  CONFIG_MESHPAY_LORA_WIOE5_UART_NUM,
                                  CONFIG_MESHPAY_LORA_WIOE5_PIN_TX,
                                  CONFIG_MESHPAY_LORA_WIOE5_PIN_RX);
}

#elif defined(CONFIG_MESHPAY_LORA_DRIVER_CORE1262)

#include "hal_lora_core1262.h"

hal_err_t hal_lora_create_default(hal_lora_t *lora)
{
    if (!lora) {
        return HAL_ERR_INVALID;
    }
    const hal_lora_core1262_pins_t pins = {
        .spi_host   = CONFIG_MESHPAY_LORA_C1262_SPI_HOST,
        .pin_sck    = CONFIG_MESHPAY_LORA_C1262_PIN_SCK,
        .pin_mosi   = CONFIG_MESHPAY_LORA_C1262_PIN_MOSI,
        .pin_miso   = CONFIG_MESHPAY_LORA_C1262_PIN_MISO,
        .pin_nss    = CONFIG_MESHPAY_LORA_C1262_PIN_NSS,
        .pin_reset  = CONFIG_MESHPAY_LORA_C1262_PIN_RESET,
        .pin_busy   = CONFIG_MESHPAY_LORA_C1262_PIN_BUSY,
        .pin_dio1   = CONFIG_MESHPAY_LORA_C1262_PIN_DIO1,
        .pin_rxen   = CONFIG_MESHPAY_LORA_C1262_PIN_RXEN,
        .pin_txen   = CONFIG_MESHPAY_LORA_C1262_PIN_TXEN,
    };
    return hal_lora_core1262_create(lora, &pins);
}

#else
#error "CONFIG_MESHPAY_LORA_DRIVER non defini — verifier components/device_hal/Kconfig"
#endif
```

- [ ] **Step 3: Brancher la factory dans le CMake de device_hal**

Modify `components/device_hal/CMakeLists.txt`. Remplacer le bloc actuel (lignes ~15-30) :

```cmake
if(CONFIG_IDF_TARGET_ESP32)
    # CYD : display ILI9341 + LoRa Wio-E5 + storage NVS
    list(APPEND HAL_SRCS
        "src/esp32/hal_storage_esp32.c"
        "src/esp32/hal_lora_wio_e5.c"
        "src/esp32/hal_display_ili9341.c"
    )
    list(APPEND HAL_PRIV_INCLUDE "src/esp32")
elseif(CONFIG_IDF_TARGET_ESP32S3)
    # Waveshare 1.47" : display JD9853 + storage NVS (pas de LoRa)
    list(APPEND HAL_SRCS
        "src/esp32/hal_storage_esp32.c"
        "src/esp32s3/hal_display_jd9853.c"
    )
    list(APPEND HAL_PRIV_INCLUDE "src/esp32s3" "src/esp32")
endif()
```

par :

```cmake
if(CONFIG_IDF_TARGET_ESP32)
    # CYD : display ILI9341 + storage NVS
    list(APPEND HAL_SRCS
        "src/esp32/hal_storage_esp32.c"
        "src/esp32/hal_display_ili9341.c"
    )
    list(APPEND HAL_PRIV_INCLUDE "src/esp32")
elseif(CONFIG_IDF_TARGET_ESP32S3)
    # Waveshare 1.47" : display JD9853 + storage NVS
    list(APPEND HAL_SRCS
        "src/esp32/hal_storage_esp32.c"
        "src/esp32s3/hal_display_jd9853.c"
    )
    list(APPEND HAL_PRIV_INCLUDE "src/esp32s3" "src/esp32")
endif()

# --- Driver LoRa : choisi par Kconfig, present sur toutes les cibles ---
# Le LoRa est obligatoire sur tous les devices ; CONFIG_MESHPAY_LORA_DRIVER
# choisit uniquement le module radio. La factory unifiee masque le choix
# au code applicatif.
list(APPEND HAL_SRCS "src/hal_lora_factory.c")
list(APPEND HAL_PRIV_INCLUDE "src/esp32")
if(CONFIG_MESHPAY_LORA_DRIVER_WIO_E5)
    list(APPEND HAL_SRCS "src/esp32/hal_lora_wio_e5.c")
elseif(CONFIG_MESHPAY_LORA_DRIVER_CORE1262)
    file(GLOB SX126X_SRCS "src/esp32/sx126x/*.c")
    list(APPEND HAL_SRCS
        ${SX126X_SRCS}
        "src/esp32/sx126x_hal.c"
        "src/esp32/core1262_params.c"
        "src/esp32/hal_lora_core1262.c"
    )
endif()
```

> Note : `list(APPEND HAL_PRIV_INCLUDE "src/esp32")` est désormais ajouté inconditionnellement (le bloc ESP32-S3 l'ajoutait déjà ; le bloc ESP32 aussi — le doublon est inoffensif pour CMake, mais on peut retirer les deux occurrences dans les blocs target pour rester DRY). Garder une seule occurrence après le bloc LoRa.

- [ ] **Step 4: Build de vérification (driver Wio-E5 par défaut)**

Run: `idf.py set-target esp32 && idf.py build`
Expected: build OK. `hal_lora_factory.c` compilé, branche `WIO_E5` active, `hal_lora_wio_e5.c` toujours compilé.

Run: `idf.py set-target esp32s3 && idf.py build`
Expected: build OK. Sur ESP32-S3 aussi, `hal_lora_factory.c` + `hal_lora_wio_e5.c` sont désormais compilés (le LoRa n'est plus gated par cible).

- [ ] **Step 5: Commit**

```bash
git add components/device_hal/include/hal/hal_lora_factory.h \
        components/device_hal/src/hal_lora_factory.c \
        components/device_hal/CMakeLists.txt
git commit -m "feat(device_hal): factory unifiee hal_lora_create_default()

Le driver LoRa est desormais choisi par Kconfig et non plus par cible.
La factory lit les GPIO/UART depuis Kconfig. Defaut Wio-E5 inchange.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>"
```

---

## Task 3: Bascule de `transport_lora.c` + suppression du stub

**Files:**
- Modify: `main/transport/transport_lora.c`
- Modify: `main/CMakeLists.txt`
- Delete: `main/transport/transport_lora_stub.c`

Milestone « refactoring pur » : zéro changement de comportement, le build Wio-E5 reste vert. C'est le dernier task avant l'ajout du Core1262.

- [ ] **Step 1: Faire pointer `transport_lora.c` sur la factory**

Modify `main/transport/transport_lora.c`.

(a) Remplacer le bloc `extern` (lignes ~30-31) :

```c
extern hal_err_t hal_lora_wio_e5_create(hal_lora_t *lora, int uart_num,
                                        int tx_pin, int rx_pin);
```

par l'include de la factory (à ajouter dans la liste d'includes, vers la ligne 26 à côté de `#include "hal/hal_lora.h"`) :

```c
#include "hal/hal_lora_factory.h"
```

…et supprimer entièrement le bloc `extern`.

(b) Supprimer les `#define` de pins Wio-E5 (lignes ~39-42), désormais dans Kconfig :

```c
/** Pins LoRa Wio-E5 (UART2) — ESP32 CYD uniquement. */
#define LORA_UART_NUM    2
#define LORA_TX_PIN     17
#define LORA_RX_PIN     16
```

→ supprimer ces 4 lignes. **Conserver** `#define LORA_SYNC_INTERVAL_MS 120000` juste en dessous.

(c) Remplacer l'appel dans `transport_lora_init_and_start()` (lignes ~144-146) :

```c
    /* 1. HAL physique. */
    hal_err_t err = hal_lora_wio_e5_create(&s_lora_hal, LORA_UART_NUM,
                                           LORA_TX_PIN, LORA_RX_PIN);
```

par :

```c
    /* 1. HAL physique — le driver concret est choisi par Kconfig. */
    hal_err_t err = hal_lora_create_default(&s_lora_hal);
```

- [ ] **Step 2: `transport_lora.c` toujours compilé dans `main/CMakeLists.txt`**

Modify `main/CMakeLists.txt`. Remplacer le bloc (lignes ~58-65) :

```cmake
# Lot D.3 : facade LoRa. Sur ESP32 (CYD) : impl reelle. Sur autres
# cibles (ESP32-S3, etc.) : stub no-op. Le code applicatif n'a plus
# aucun `#ifdef MP_HAS_LORA` autour des appels.
if(CONFIG_IDF_TARGET_ESP32)
    list(APPEND meshpay_main_srcs "transport/transport_lora.c")
else()
    list(APPEND meshpay_main_srcs "transport/transport_lora_stub.c")
endif()
```

par :

```cmake
# Facade LoRa. Le LoRa est obligatoire sur tous les devices : l'impl
# reelle est toujours compilee. Le driver radio concret (Wio-E5 /
# Core1262) est choisi par CONFIG_MESHPAY_LORA_DRIVER dans device_hal.
list(APPEND meshpay_main_srcs "transport/transport_lora.c")
```

- [ ] **Step 3: Supprimer le stub**

Run:
```bash
git rm main/transport/transport_lora_stub.c
```
Justification : le `test_app/` ne référence pas `main/` (il réutilise uniquement `../components/*`), et plus aucun `CMakeLists.txt` ne sélectionne le stub.

- [ ] **Step 4: Build de vérification — refactoring pur, comportement inchangé**

Run: `idf.py set-target esp32 && idf.py build`
Expected: build OK. Le firmware ESP32 (CYD) compile avec le driver Wio-E5, comme avant.

Run: `idf.py set-target esp32s3 && idf.py build`
Expected: build OK. Le firmware ESP32-S3 compile désormais `transport_lora.c` (impl réelle) avec le driver Wio-E5 par défaut — c'est attendu, le défaut Core1262 sera posé en Task 8.

- [ ] **Step 5: Commit**

```bash
git add main/transport/transport_lora.c main/CMakeLists.txt
git commit -m "refactor(transport_lora): passer par hal_lora_create_default()

transport_lora.c devient agnostique du driver radio. L'impl reelle est
toujours compilee (LoRa obligatoire) ; le stub no-op est supprime. Pins
Wio-E5 migrees en Kconfig. Refactoring pur, comportement inchange.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>"
```

---

## Task 4: Vendoring du pilote Semtech `sx126x_driver`

**Files:**
- Create: `components/device_hal/src/esp32/sx126x/` (fichiers du dépôt Semtech)

Le pilote Semtech est vendoré tel quel (non modifié) → mise à jour facile. Licence BSD-3-Clause.

- [ ] **Step 1: Récupérer le dépôt Semtech**

Run:
```bash
git clone --depth 1 https://github.com/Lora-net/sx126x_driver.git /tmp/sx126x_driver
```
Expected: clone OK. Le dossier `/tmp/sx126x_driver/src/` contient `sx126x.c`, `sx126x.h`, `sx126x_regs.h`, `sx126x_hal.h` et les fichiers LR-FHSS (`lr_fhss_mac.*`, `sx126x_lr_fhss.*`).

- [ ] **Step 2: Copier `src/` dans le composant**

Run:
```bash
mkdir -p "components/device_hal/src/esp32/sx126x"
cp /tmp/sx126x_driver/src/*.c /tmp/sx126x_driver/src/*.h \
   "components/device_hal/src/esp32/sx126x/"
cp /tmp/sx126x_driver/LICENSE "components/device_hal/src/esp32/sx126x/LICENSE"
ls components/device_hal/src/esp32/sx126x/
```
Expected: liste contenant au minimum `sx126x.c`, `sx126x.h`, `sx126x_regs.h`, `sx126x_hal.h`, `LICENSE`.

> Si `sx126x.h` `#include "sx126x_status.h"` mais que ce fichier n'est pas présent dans `src/`, c'est que cette version du driver définit `sx126x_status_t` directement dans `sx126x.h` — dans ce cas l'`#include` n'existe pas non plus. Copier tout `src/` couvre les deux cas de figure.

- [ ] **Step 3: Vérifier que les fichiers vendorés compilent (driver toujours Wio-E5)**

Run: `idf.py set-target esp32s3 && idf.py build`
Expected: build OK. Les fichiers `sx126x/*.c` ne sont **pas encore** compilés (le `file(GLOB)` n'est dans la branche `CORE1262` du CMake, inactive tant que le défaut est `WIO_E5`). Ce build vérifie juste qu'aucune copie n'a cassé l'arborescence.

- [ ] **Step 4: Commit**

```bash
git add components/device_hal/src/esp32/sx126x/
git commit -m "chore(device_hal): vendoring du pilote Semtech sx126x_driver

Pilote registre SX126x officiel (BSD-3-Clause), copie telle quelle
depuis github.com/Lora-net/sx126x_driver. Pas encore compile.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>"
```

---

## Task 5: Mapper de configuration radio (TDD)

**Files:**
- Create: `components/device_hal/test/test_core1262_params.c`
- Create: `components/device_hal/src/esp32/core1262_params.h`
- Create: `components/device_hal/src/esp32/core1262_params.c`
- Modify: `components/device_hal/CMakeLists.txt`

C'est l'unique logique pure et testable du backend : la traduction `hal_lora_config_t` → paramètres SX1262. On la développe en TDD.

- [ ] **Step 1: Écrire le test qui échoue**

Create `components/device_hal/test/test_core1262_params.c`:

```c
/**
 * @file test_core1262_params.c
 * @brief Tests unitaires du mapper core1262_map_config().
 *
 * Verifie la traduction hal_lora_config_t -> parametres SX1262 :
 * spreading factor, bande passante, coding rate, LDRO, frequence,
 * puissance, et le rejet des valeurs hors plage.
 */

#include "unity.h"
#include "core1262_params.h"
#include "hal/hal_lora.h"

/* Config de reference du projet : 868.1 MHz, SF9, BW125, CR4/5, 14 dBm. */
static hal_lora_config_t base_config(void)
{
    hal_lora_config_t c = {
        .frequency_hz     = 868100000,
        .spreading_factor = 9,
        .bandwidth        = 0,   /* 0 = 125 kHz */
        .coding_rate      = 1,   /* 1 = 4/5 */
        .tx_power_dbm     = 14,
    };
    return c;
}

TEST_CASE("core1262_params : config de reference mappee correctement", "[core1262]")
{
    hal_lora_config_t cfg = base_config();
    core1262_radio_params_t out;

    TEST_ASSERT_TRUE(core1262_map_config(&cfg, &out));
    TEST_ASSERT_EQUAL(SX126X_LORA_SF9,    out.mod.sf);
    TEST_ASSERT_EQUAL(SX126X_LORA_BW_125, out.mod.bw);
    TEST_ASSERT_EQUAL(SX126X_LORA_CR_4_5, out.mod.cr);
    TEST_ASSERT_EQUAL_UINT8(0,            out.mod.ldro);   /* SF9/BW125 : LDRO off */
    TEST_ASSERT_EQUAL_UINT32(868100000,   out.freq_hz);
    TEST_ASSERT_EQUAL_INT8(14,            out.power_dbm);
}

TEST_CASE("core1262_params : mapping des trois bandes passantes", "[core1262]")
{
    hal_lora_config_t cfg = base_config();
    core1262_radio_params_t out;

    cfg.bandwidth = 0;
    TEST_ASSERT_TRUE(core1262_map_config(&cfg, &out));
    TEST_ASSERT_EQUAL(SX126X_LORA_BW_125, out.mod.bw);

    cfg.bandwidth = 1;
    TEST_ASSERT_TRUE(core1262_map_config(&cfg, &out));
    TEST_ASSERT_EQUAL(SX126X_LORA_BW_250, out.mod.bw);

    cfg.bandwidth = 2;
    TEST_ASSERT_TRUE(core1262_map_config(&cfg, &out));
    TEST_ASSERT_EQUAL(SX126X_LORA_BW_500, out.mod.bw);
}

TEST_CASE("core1262_params : mapping des quatre coding rates", "[core1262]")
{
    hal_lora_config_t cfg = base_config();
    core1262_radio_params_t out;

    cfg.coding_rate = 1;
    TEST_ASSERT_TRUE(core1262_map_config(&cfg, &out));
    TEST_ASSERT_EQUAL(SX126X_LORA_CR_4_5, out.mod.cr);

    cfg.coding_rate = 4;
    TEST_ASSERT_TRUE(core1262_map_config(&cfg, &out));
    TEST_ASSERT_EQUAL(SX126X_LORA_CR_4_8, out.mod.cr);
}

TEST_CASE("core1262_params : LDRO active pour SF11/SF12 en BW125", "[core1262]")
{
    hal_lora_config_t cfg = base_config();
    core1262_radio_params_t out;

    cfg.spreading_factor = 12;
    cfg.bandwidth = 0;  /* 125 kHz */
    TEST_ASSERT_TRUE(core1262_map_config(&cfg, &out));
    TEST_ASSERT_EQUAL_UINT8(1, out.mod.ldro);

    cfg.spreading_factor = 11;
    TEST_ASSERT_TRUE(core1262_map_config(&cfg, &out));
    TEST_ASSERT_EQUAL_UINT8(1, out.mod.ldro);

    /* SF12 en BW500 : symbole court, LDRO off. */
    cfg.spreading_factor = 12;
    cfg.bandwidth = 2;
    TEST_ASSERT_TRUE(core1262_map_config(&cfg, &out));
    TEST_ASSERT_EQUAL_UINT8(0, out.mod.ldro);
}

TEST_CASE("core1262_params : parametres de paquet par defaut", "[core1262]")
{
    hal_lora_config_t cfg = base_config();
    core1262_radio_params_t out;

    TEST_ASSERT_TRUE(core1262_map_config(&cfg, &out));
    TEST_ASSERT_EQUAL_UINT16(8, out.pkt.preamble_len_in_symb);
    TEST_ASSERT_EQUAL(SX126X_LORA_PKT_EXPLICIT, out.pkt.header_type);
    TEST_ASSERT_TRUE(out.pkt.crc_is_on);
    TEST_ASSERT_FALSE(out.pkt.invert_iq_is_on);
}

TEST_CASE("core1262_params : rejet des valeurs hors plage", "[core1262]")
{
    core1262_radio_params_t out;
    hal_lora_config_t cfg;

    /* SF hors [7,12] */
    cfg = base_config(); cfg.spreading_factor = 6;
    TEST_ASSERT_FALSE(core1262_map_config(&cfg, &out));
    cfg = base_config(); cfg.spreading_factor = 13;
    TEST_ASSERT_FALSE(core1262_map_config(&cfg, &out));

    /* BW hors [0,2] */
    cfg = base_config(); cfg.bandwidth = 3;
    TEST_ASSERT_FALSE(core1262_map_config(&cfg, &out));

    /* CR hors [1,4] */
    cfg = base_config(); cfg.coding_rate = 0;
    TEST_ASSERT_FALSE(core1262_map_config(&cfg, &out));
    cfg = base_config(); cfg.coding_rate = 5;
    TEST_ASSERT_FALSE(core1262_map_config(&cfg, &out));

    /* Pointeurs NULL */
    cfg = base_config();
    TEST_ASSERT_FALSE(core1262_map_config(NULL, &out));
    TEST_ASSERT_FALSE(core1262_map_config(&cfg, NULL));
}
```

- [ ] **Step 2: Créer l'en-tête du mapper**

Create `components/device_hal/src/esp32/core1262_params.h`:

```c
/**
 * @file core1262_params.h
 * @brief Traduction hal_lora_config_t -> parametres radio SX1262.
 *
 * Logique pure (sans dependance ESP-IDF), testable unitairement.
 * Le backend hal_lora_core1262.c consomme le resultat pour configurer
 * le SX1262 via le pilote Semtech.
 */

#ifndef CORE1262_PARAMS_H
#define CORE1262_PARAMS_H

#include <stdbool.h>
#include <stdint.h>

#include "hal/hal_lora.h"
#include "sx126x.h"

/**
 * Parametres radio SX1262 derives d'un hal_lora_config_t.
 *
 * Le champ pkt.pld_len_in_bytes est laisse a 0 : il est renseigne
 * a chaque emission/reception (taille variable du paquet).
 */
typedef struct {
    sx126x_mod_params_lora_t mod;        /**< SF, BW, CR, LDRO */
    sx126x_pkt_params_lora_t pkt;        /**< preambule, header, CRC, IQ */
    uint32_t                 freq_hz;    /**< Frequence porteuse */
    int8_t                   power_dbm;  /**< Puissance d'emission */
} core1262_radio_params_t;

/**
 * Traduit une config HAL portable en parametres SX1262.
 *
 * @param cfg Configuration HAL (freq, SF 7-12, BW 0-2, CR 1-4, puissance)
 * @param out [out] Parametres SX1262 remplis
 * @return true si la config est valide, false si une valeur est hors
 *         plage ou si un pointeur est NULL
 */
bool core1262_map_config(const hal_lora_config_t *cfg,
                         core1262_radio_params_t *out);

#endif /* CORE1262_PARAMS_H */
```

- [ ] **Step 3: Implémenter le mapper**

Create `components/device_hal/src/esp32/core1262_params.c`:

```c
/**
 * @file core1262_params.c
 * @brief Implementation du mapper hal_lora_config_t -> SX1262.
 */

#include "core1262_params.h"

/* Plages valides cote HAL. */
#define SF_MIN  7
#define SF_MAX  12
#define BW_MAX  2   /* 0=125, 1=250, 2=500 */
#define CR_MIN  1   /* 1=4/5 */
#define CR_MAX  4   /* 4=4/8 */

/* Plage de puissance du SX1262 (PA haute puissance). */
#define POWER_MIN_DBM  (-9)
#define POWER_MAX_DBM  (22)

bool core1262_map_config(const hal_lora_config_t *cfg,
                         core1262_radio_params_t *out)
{
    if (!cfg || !out) {
        return false;
    }

    /* --- Spreading factor : l'enum SX126X_LORA_SFx vaut numeriquement SFx. --- */
    if (cfg->spreading_factor < SF_MIN || cfg->spreading_factor > SF_MAX) {
        return false;
    }
    out->mod.sf = (sx126x_lora_sf_t)cfg->spreading_factor;

    /* --- Bande passante : 0/1/2 -> 125/250/500 kHz. --- */
    switch (cfg->bandwidth) {
        case 0:  out->mod.bw = SX126X_LORA_BW_125; break;
        case 1:  out->mod.bw = SX126X_LORA_BW_250; break;
        case 2:  out->mod.bw = SX126X_LORA_BW_500; break;
        default: return false;
    }

    /* --- Coding rate : l'enum SX126X_LORA_CR_4_x vaut numeriquement x (1-4). --- */
    if (cfg->coding_rate < CR_MIN || cfg->coding_rate > CR_MAX) {
        return false;
    }
    out->mod.cr = (sx126x_lora_cr_t)cfg->coding_rate;

    /*
     * --- LDRO (Low Data Rate Optimize) ---
     * Recommande par Semtech quand la duree d'un symbole depasse ~16 ms,
     * ce qui arrive aux SF eleves sur bande etroite. Regle pratique :
     * actif pour SF11 et SF12 en BW125 kHz (cfg->bandwidth == 0).
     */
    out->mod.ldro = ((cfg->spreading_factor >= 11) && (cfg->bandwidth == 0))
                        ? 1 : 0;

    /* --- Parametres de paquet LoRa : header explicite, CRC actif. --- */
    out->pkt.preamble_len_in_symb = 8;
    out->pkt.header_type          = SX126X_LORA_PKT_EXPLICIT;
    out->pkt.pld_len_in_bytes     = 0;  /* renseigne a chaque TX/RX */
    out->pkt.crc_is_on            = true;
    out->pkt.invert_iq_is_on      = false;

    /* --- Frequence : passe-plat. --- */
    out->freq_hz = cfg->frequency_hz;

    /* --- Puissance : bornee a la plage du SX1262. --- */
    int8_t pwr = cfg->tx_power_dbm;
    if (pwr < POWER_MIN_DBM) pwr = POWER_MIN_DBM;
    if (pwr > POWER_MAX_DBM) pwr = POWER_MAX_DBM;
    out->power_dbm = pwr;

    return true;
}
```

- [ ] **Step 4: Ajouter `core1262_params.c` et son test au CMake (build CORE1262 uniquement)**

Modify `components/device_hal/CMakeLists.txt`.

(a) `core1262_params.c` est déjà listé dans la branche `CORE1262` ajoutée en Task 2 — vérifier sa présence, sinon l'ajouter.

(b) Ajouter le fichier de test dans le bloc test (`if(project_name STREQUAL "meshpay_test_app")`), conditionné au driver Core1262 :

```cmake
if(project_name STREQUAL "meshpay_test_app")
    # Tests : ajout du mock storage + test_hal_storage.c
    list(APPEND HAL_SRCS
        "src/mock/hal_storage_mock.c"
        "src/mock/hal_display_mock.c"
        "src/mock/hal_lora_mock.c"
        "test/test_hal_storage.c"
        "test/test_hal_power.c"
    )
    # Test du mapper Core1262 : uniquement quand ce driver est compile.
    if(CONFIG_MESHPAY_LORA_DRIVER_CORE1262)
        list(APPEND HAL_SRCS "test/test_core1262_params.c")
    endif()
    list(APPEND HAL_PRIV_INCLUDE "src/mock")
    ...
```

- [ ] **Step 5: Lancer le test et vérifier qu'il échoue d'abord, puis passe**

Le test ne se compile que sous le driver Core1262. Sélectionner Core1262 temporairement :
```bash
idf.py -C test_app set-target esp32s3
idf.py -C test_app menuconfig   # Mesh Pay - LoRa -> Driver radio LoRa -> Core1262
```

D'abord, vérifier l'échec attendu : renommer temporairement le corps de `core1262_map_config` pour qu'il retourne toujours `false` n'est pas nécessaire — si le mapper a été implémenté à l'étape 3, passer directement au build.

Run: `idf.py -C test_app build && idf.py -C test_app -p <PORT> flash monitor`
Au menu Unity série, lancer `[core1262]`.
Expected: les 6 `TEST_CASE` `[core1262]` passent.

> Si l'implémentation a une erreur, le test échoue ici — corriger `core1262_params.c` et rebuilder. Ne pas marquer le step complété tant que les 6 tests ne passent pas.

- [ ] **Step 6: Commit**

```bash
git add components/device_hal/src/esp32/core1262_params.h \
        components/device_hal/src/esp32/core1262_params.c \
        components/device_hal/test/test_core1262_params.c \
        components/device_hal/CMakeLists.txt
git commit -m "feat(device_hal): mapper de config radio Core1262 + tests

core1262_map_config() traduit hal_lora_config_t en parametres SX1262
(SF/BW/CR/LDRO). Logique pure, 6 tests unitaires Unity.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>"
```

---

## Task 6: Glue plateforme SPI/GPIO (`sx126x_hal.c`)

**Files:**
- Create: `components/device_hal/src/esp32/sx126x_hal_context.h`
- Create: `components/device_hal/src/esp32/sx126x_hal.c`

Le pilote Semtech appelle 4 fonctions plateforme (`sx126x_hal_write/read/reset/wakeup`) déclarées dans `sx126x/sx126x_hal.h`. On les implémente avec l'API SPI/GPIO d'ESP-IDF. Le `context` opaque que le pilote transmet est notre struct `core1262_hw_t`.

- [ ] **Step 1: Créer la struct matérielle partagée**

Create `components/device_hal/src/esp32/sx126x_hal_context.h`:

```c
/**
 * @file sx126x_hal_context.h
 * @brief Contexte materiel passe au pilote Semtech comme `context` opaque.
 *
 * Partage entre sx126x_hal.c (la glue) et hal_lora_core1262.c (le backend
 * qui cree le handle SPI et configure les GPIO). Le pilote Semtech reçoit
 * un `const void *context` qu'il transmet tel quel a la glue.
 */

#ifndef SX126X_HAL_CONTEXT_H
#define SX126X_HAL_CONTEXT_H

#include "driver/spi_master.h"

/** Ressources materielles necessaires aux transactions SX1262. */
typedef struct {
    spi_device_handle_t spi;        /**< Handle du device SPI (CS gere a la main) */
    int                 pin_nss;    /**< GPIO chip-select (pilote a la main) */
    int                 pin_busy;   /**< GPIO BUSY (entree) */
    int                 pin_reset;  /**< GPIO RESET (sortie) */
} core1262_hw_t;

#endif /* SX126X_HAL_CONTEXT_H */
```

- [ ] **Step 2: Implémenter la glue**

Create `components/device_hal/src/esp32/sx126x_hal.c`:

```c
/**
 * @file sx126x_hal.c
 * @brief Glue plateforme ESP-IDF pour le pilote Semtech sx126x.
 *
 * Implemente les 4 fonctions sx126x_hal_* attendues par sx126x/sx126x.c :
 * transactions SPI (write/read), reset materiel, reveil de veille.
 *
 * Protocole SX126x : NSS bas, opcode + adresse + data, NSS haut. Avant
 * chaque transaction il faut attendre que BUSY repasse a 0. Le CS n'est
 * pas confie au pilote SPI (spics_io_num = -1 cote backend) : on le
 * pilote ici a la main pour englober opcode + data dans une seule
 * sequence NSS bas..haut.
 */

#include "sx126x_hal.h"
#include "sx126x_hal_context.h"

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_rom_sys.h"
#include "esp_log.h"

static const char *TAG = "sx126x_hal";

/** Timeout d'attente du pin BUSY (ms). */
#define BUSY_TIMEOUT_MS 100

/**
 * Attend que le SX1262 libere BUSY (niveau bas = pret).
 *
 * @param hw Contexte materiel
 * @return SX126X_HAL_STATUS_OK si BUSY est bas, _ERROR sur timeout
 */
static sx126x_hal_status_t wait_on_busy(const core1262_hw_t *hw)
{
    int waited_us = 0;
    while (gpio_get_level(hw->pin_busy) == 1) {
        esp_rom_delay_us(10);
        waited_us += 10;
        if (waited_us >= BUSY_TIMEOUT_MS * 1000) {
            ESP_LOGE(TAG, "Timeout BUSY (%d ms)", BUSY_TIMEOUT_MS);
            return SX126X_HAL_STATUS_ERROR;
        }
    }
    return SX126X_HAL_STATUS_OK;
}

sx126x_hal_status_t sx126x_hal_write(const void *context,
                                     const uint8_t *command,
                                     const uint16_t command_length,
                                     const uint8_t *data,
                                     const uint16_t data_length)
{
    const core1262_hw_t *hw = (const core1262_hw_t *)context;

    if (wait_on_busy(hw) != SX126X_HAL_STATUS_OK) {
        return SX126X_HAL_STATUS_ERROR;
    }

    /* NSS bas pour toute la transaction (opcode + data). */
    gpio_set_level(hw->pin_nss, 0);

    esp_err_t err = ESP_OK;

    /* 1. Envoi de l'opcode + adresse. */
    spi_transaction_t t_cmd = {
        .length    = (size_t)command_length * 8,
        .tx_buffer = command,
    };
    err = spi_device_polling_transmit(hw->spi, &t_cmd);

    /* 2. Envoi des donnees (si presentes). */
    if (err == ESP_OK && data_length > 0) {
        spi_transaction_t t_data = {
            .length    = (size_t)data_length * 8,
            .tx_buffer = data,
        };
        err = spi_device_polling_transmit(hw->spi, &t_data);
    }

    gpio_set_level(hw->pin_nss, 1);

    return (err == ESP_OK) ? SX126X_HAL_STATUS_OK : SX126X_HAL_STATUS_ERROR;
}

sx126x_hal_status_t sx126x_hal_read(const void *context,
                                    const uint8_t *command,
                                    const uint16_t command_length,
                                    uint8_t *data,
                                    const uint16_t data_length)
{
    const core1262_hw_t *hw = (const core1262_hw_t *)context;

    if (wait_on_busy(hw) != SX126X_HAL_STATUS_OK) {
        return SX126X_HAL_STATUS_ERROR;
    }

    gpio_set_level(hw->pin_nss, 0);

    esp_err_t err = ESP_OK;

    /* 1. Envoi de l'opcode + adresse (les octets de status sortis sont ignores). */
    spi_transaction_t t_cmd = {
        .length    = (size_t)command_length * 8,
        .tx_buffer = command,
    };
    err = spi_device_polling_transmit(hw->spi, &t_cmd);

    /* 2. Lecture des donnees : on cadence des octets factices et on capture MISO. */
    if (err == ESP_OK && data_length > 0) {
        spi_transaction_t t_data = {
            .length    = (size_t)data_length * 8,
            .rxlength  = (size_t)data_length * 8,
            .tx_buffer = NULL,           /* octets sortants = 0 (NOP) */
            .rx_buffer = data,
        };
        err = spi_device_polling_transmit(hw->spi, &t_data);
    }

    gpio_set_level(hw->pin_nss, 1);

    return (err == ESP_OK) ? SX126X_HAL_STATUS_OK : SX126X_HAL_STATUS_ERROR;
}

sx126x_hal_status_t sx126x_hal_reset(const void *context)
{
    const core1262_hw_t *hw = (const core1262_hw_t *)context;

    /* Reset materiel : RESET bas >= 100 us, puis haut, puis attente BUSY. */
    gpio_set_level(hw->pin_reset, 0);
    esp_rom_delay_us(200);
    gpio_set_level(hw->pin_reset, 1);
    vTaskDelay(pdMS_TO_TICKS(10));   /* laisser le SX1262 redemarrer */

    return wait_on_busy(hw);
}

sx126x_hal_status_t sx126x_hal_wakeup(const void *context)
{
    const core1262_hw_t *hw = (const core1262_hw_t *)context;

    /* Sortie de veille : un front descendant sur NSS reveille le SX1262. */
    gpio_set_level(hw->pin_nss, 0);
    esp_rom_delay_us(20);
    gpio_set_level(hw->pin_nss, 1);

    return wait_on_busy(hw);
}
```

- [ ] **Step 3: Commit** (build vérifié en Task 7, la glue seule n'est pas encore compilée)

```bash
git add components/device_hal/src/esp32/sx126x_hal_context.h \
        components/device_hal/src/esp32/sx126x_hal.c
git commit -m "feat(device_hal): glue plateforme SPI/GPIO pour le pilote sx126x

Implemente sx126x_hal_write/read/reset/wakeup avec spi_master + gpio
ESP-IDF. CS pilote a la main, attente BUSY avant chaque transaction.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>"
```

---

## Task 7: Backend HAL `hal_lora_core1262.c`

**Files:**
- Create: `components/device_hal/src/esp32/hal_lora_core1262.h`
- Create: `components/device_hal/src/esp32/hal_lora_core1262.c`
- Modify: `components/device_hal/CMakeLists.txt` (vérifier la branche `CORE1262`, déjà posée en Task 2)

Le backend remplit la vtable `hal_lora_t`. Il crée le bus + device SPI, configure les GPIO, fait le bring-up radio via le pilote Semtech, et gère une tâche RX déclenchée par l'IRQ DIO1.

> Rappel honnête : la séquence de bring-up RF (config PA, tension TCXO) suit la doc Semtech mais peut demander un ajustement au smoke test (Task 9).

- [ ] **Step 1: Créer l'en-tête de la factory**

Create `components/device_hal/src/esp32/hal_lora_core1262.h`:

```c
/**
 * @file hal_lora_core1262.h
 * @brief Factory du backend LoRa Waveshare Core1262 (SX1262 SPI).
 */

#ifndef HAL_LORA_CORE1262_H
#define HAL_LORA_CORE1262_H

#include "hal/hal_lora.h"

/** Brochage du module Core1262 cote ESP32 (issu de Kconfig). */
typedef struct {
    int spi_host;    /**< Hote SPI (ex: 2 pour SPI3_HOST) */
    int pin_sck;     /**< GPIO SCK */
    int pin_mosi;    /**< GPIO MOSI */
    int pin_miso;    /**< GPIO MISO */
    int pin_nss;     /**< GPIO NSS (chip-select) */
    int pin_reset;   /**< GPIO RESET */
    int pin_busy;    /**< GPIO BUSY */
    int pin_dio1;    /**< GPIO DIO1 (IRQ) */
    int pin_rxen;    /**< GPIO RXEN (switch RF RX) */
    int pin_txen;    /**< GPIO TXEN (switch RF TX) */
} hal_lora_core1262_pins_t;

/**
 * Cree une instance LoRa pour le module Waveshare Core1262.
 *
 * @param lora [out] Vtable a remplir
 * @param pins Brochage du module
 * @return HAL_OK en cas de succes, HAL_ERR_INVALID si un argument est NULL
 */
hal_err_t hal_lora_core1262_create(hal_lora_t *lora,
                                   const hal_lora_core1262_pins_t *pins);

#endif /* HAL_LORA_CORE1262_H */
```

- [ ] **Step 2: Implémenter le backend**

Create `components/device_hal/src/esp32/hal_lora_core1262.c`:

```c
/**
 * @file hal_lora_core1262.c
 * @brief Backend HAL LoRa pour le module Waveshare Core1262 (SX1262 SPI).
 *
 * Remplit la vtable hal_lora_t en pilotant le SX1262 via le pilote
 * Semtech vendore (sx126x/) et la glue plateforme (sx126x_hal.c).
 *
 * Architecture :
 * - init : bus + device SPI, GPIO, reset, TCXO (DIO3), calibration,
 *   config LoRa (frequence/PA/modulation), routage IRQ.
 * - send : bloquant. Bascule le switch RF en TX, ecrit le buffer,
 *   lance l'emission, attend TX_DONE en scrutant le registre IRQ.
 * - reception : la tache rx_task est reveillee par l'ISR du pin DIO1
 *   (routage IRQ : seul RX_DONE/TIMEOUT est route sur DIO1). Elle lit
 *   le paquet et appelle le callback applicatif.
 * - Un mutex serialise tous les acces radio entre send() et rx_task.
 *
 * Switch RF : le module Core1262 expose RXEN/TXEN ; on les pilote a la
 * main (RXEN=1/TXEN=0 en reception, l'inverse en emission).
 */

#include "hal_lora_core1262.h"
#include "hal_lora_core1262_internal_unused.h" /* placeholder retire ci-dessous */
#include "sx126x_hal_context.h"
#include "core1262_params.h"
#include "sx126x.h"

#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"

#include <string.h>

static const char *TAG = "hal_lora_c1262";

/* ================================================================
 * Constantes
 * ================================================================ */

/** Horloge SPI : 8 MHz (le SX1262 supporte 18 MHz ; marge pour fils volants). */
#define C1262_SPI_CLOCK_HZ   (8 * 1000 * 1000)

/** Tension de controle du TCXO via DIO3. A confirmer au smoke test. */
#define C1262_TCXO_VOLTAGE   SX126X_TCXO_CTRL_1_8V
/** Delai de stabilisation du TCXO, en pas RTC de 15.625 us (~5 ms). */
#define C1262_TCXO_TIMEOUT   320

/** Stack et priorite de la tache RX. */
#define C1262_RX_TASK_STACK  4096
#define C1262_RX_TASK_PRIO   5

/** Timeout d'attente de TX_DONE (ms). */
#define C1262_TX_TIMEOUT_MS  4000

/* ================================================================
 * Contexte interne
 * ================================================================ */

typedef struct {
    core1262_hw_t           hw;           /**< SPI + NSS/BUSY/RESET (passe au pilote Semtech) */
    int                     pin_dio1;     /**< GPIO IRQ */
    int                     pin_rxen;     /**< GPIO switch RF RX */
    int                     pin_txen;     /**< GPIO switch RF TX */
    int                     spi_host;     /**< Hote SPI */
    int                     pin_sck;
    int                     pin_mosi;
    int                     pin_miso;

    core1262_radio_params_t params;       /**< Parametres radio derives de la config */

    bool                    initialized;
    bool                    rx_running;
    hal_lora_rx_cb_t        rx_cb;
    void                   *rx_user_ctx;

    SemaphoreHandle_t       radio_mutex;  /**< Serialise les acces radio */
    SemaphoreHandle_t       dio1_sem;     /**< Donne par l'ISR DIO1 (RX_DONE) */
    TaskHandle_t            rx_task_handle;
} core1262_ctx_t;

static core1262_ctx_t s_ctx;

/* ================================================================
 * Helpers bas niveau
 * ================================================================ */

/** Bascule le switch RF en mode reception. */
static void rf_switch_rx(core1262_ctx_t *ctx)
{
    gpio_set_level(ctx->pin_txen, 0);
    gpio_set_level(ctx->pin_rxen, 1);
}

/** Bascule le switch RF en mode emission. */
static void rf_switch_tx(core1262_ctx_t *ctx)
{
    gpio_set_level(ctx->pin_rxen, 0);
    gpio_set_level(ctx->pin_txen, 1);
}

/** ISR du pin DIO1 : reveille la tache RX. */
static void IRAM_ATTR dio1_isr_handler(void *arg)
{
    core1262_ctx_t *ctx = (core1262_ctx_t *)arg;
    BaseType_t hp_task_woken = pdFALSE;
    xSemaphoreGiveFromISR(ctx->dio1_sem, &hp_task_woken);
    if (hp_task_woken == pdTRUE) {
        portYIELD_FROM_ISR();
    }
}

/**
 * Applique toute la config radio sur le SX1262 (hors taille de paquet).
 * Appele une fois a l'init. Retourne HAL_OK / HAL_ERR_IO.
 */
static hal_err_t apply_radio_config(core1262_ctx_t *ctx)
{
    const void *sx = &ctx->hw;
    sx126x_status_t st;

    /* Sortie de reset -> standby RC. */
    st = sx126x_set_standby(sx, SX126X_STANDBY_CFG_RC);
    if (st != SX126X_STATUS_OK) return HAL_ERR_IO;

    /* Le module Core1262 a un TCXO alimente par DIO3 : le declarer AVANT
     * la calibration, sinon la calibration PLL echoue. */
    st = sx126x_set_dio3_as_tcxo_ctrl(sx, C1262_TCXO_VOLTAGE, C1262_TCXO_TIMEOUT);
    if (st != SX126X_STATUS_OK) return HAL_ERR_IO;

    /* Calibration complete (RC, PLL, ADC, image). */
    st = sx126x_cal(sx, SX126X_CAL_ALL);
    if (st != SX126X_STATUS_OK) return HAL_ERR_IO;

    /* Le switch RF est pilote par RXEN/TXEN (GPIO), pas par DIO2 : on
     * desactive explicitement le controle DIO2. */
    st = sx126x_set_dio2_as_rf_sw_ctrl(sx, false);
    if (st != SX126X_STATUS_OK) return HAL_ERR_IO;

    /* Type de paquet : LoRa. */
    st = sx126x_set_pkt_type(sx, SX126X_PKT_TYPE_LORA);
    if (st != SX126X_STATUS_OK) return HAL_ERR_IO;

    /* Frequence porteuse. */
    st = sx126x_set_rf_freq(sx, ctx->params.freq_hz);
    if (st != SX126X_STATUS_OK) return HAL_ERR_IO;

    /* Configuration de l'amplificateur de puissance (SX1262, +14 dBm).
     * Valeurs issues de la table Semtech AN1200.x pour le SX1262. A
     * reverifier au smoke test si la portee est anormale. */
    const sx126x_pa_cfg_params_t pa_cfg = {
        .pa_duty_cycle = 0x02,
        .hp_max        = 0x02,
        .device_sel    = 0x00,  /* 0x00 = SX1262 */
        .pa_lut        = 0x01,
    };
    st = sx126x_set_pa_cfg(sx, &pa_cfg);
    if (st != SX126X_STATUS_OK) return HAL_ERR_IO;

    st = sx126x_set_tx_params(sx, ctx->params.power_dbm, SX126X_RAMP_200_US);
    if (st != SX126X_STATUS_OK) return HAL_ERR_IO;

    /* Parametres de modulation LoRa (SF/BW/CR/LDRO). */
    st = sx126x_set_lora_mod_params(sx, &ctx->params.mod);
    if (st != SX126X_STATUS_OK) return HAL_ERR_IO;

    /* Adresses de base des buffers TX/RX dans la FIFO du SX1262. */
    st = sx126x_set_buffer_base_address(sx, 0x00, 0x00);
    if (st != SX126X_STATUS_OK) return HAL_ERR_IO;

    /* Routage des IRQ : TX_DONE/RX_DONE/TIMEOUT actives ; seuls
     * RX_DONE et TIMEOUT sont routes sur le pin DIO1. TX_DONE reste
     * lisible dans le registre de status (scrute par send()). */
    const uint16_t irq_mask  = SX126X_IRQ_TX_DONE | SX126X_IRQ_RX_DONE |
                               SX126X_IRQ_TIMEOUT;
    const uint16_t dio1_mask = SX126X_IRQ_RX_DONE | SX126X_IRQ_TIMEOUT;
    st = sx126x_set_dio_irq_params(sx, irq_mask, dio1_mask, 0x0000, 0x0000);
    if (st != SX126X_STATUS_OK) return HAL_ERR_IO;

    return HAL_OK;
}

/* ================================================================
 * Tache de reception
 * ================================================================ */

static void c1262_rx_task(void *param)
{
    core1262_ctx_t *ctx = (core1262_ctx_t *)param;
    const void *sx = &ctx->hw;
    uint8_t pkt_buf[HAL_LORA_MAX_PACKET_SIZE];

    ESP_LOGI(TAG, "Tache RX demarree");

    while (ctx->rx_running) {
        /* Attendre une IRQ DIO1 (RX_DONE/TIMEOUT). Reveil periodique pour
         * pouvoir sortir proprement si rx_running passe a false. */
        if (xSemaphoreTake(ctx->dio1_sem, pdMS_TO_TICKS(500)) != pdTRUE) {
            continue;
        }

        if (xSemaphoreTake(ctx->radio_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
            ESP_LOGW(TAG, "RX : mutex radio indisponible");
            continue;
        }

        sx126x_irq_mask_t irq = 0;
        sx126x_get_irq_status(sx, &irq);
        sx126x_clear_irq_status(sx, irq);

        int     pkt_len = -1;
        int16_t rssi    = 0;

        if (irq & SX126X_IRQ_RX_DONE) {
            sx126x_rx_buffer_status_t rxb = {0};
            if (sx126x_get_rx_buffer_status(sx, &rxb) == SX126X_STATUS_OK &&
                rxb.pld_len_in_bytes > 0 &&
                rxb.pld_len_in_bytes <= HAL_LORA_MAX_PACKET_SIZE) {

                if (sx126x_read_buffer(sx, rxb.buffer_start_pointer, pkt_buf,
                                       rxb.pld_len_in_bytes) == SX126X_STATUS_OK) {
                    pkt_len = rxb.pld_len_in_bytes;

                    sx126x_pkt_status_lora_t pst = {0};
                    if (sx126x_get_lora_pkt_status(sx, &pst) == SX126X_STATUS_OK) {
                        rssi = pst.rssi_pkt_in_dbm;
                    }
                }
            }
        }

        /* Re-armer la reception continue (timeout 0 = continu). */
        rf_switch_rx(ctx);
        sx126x_set_rx(sx, 0);

        xSemaphoreGive(ctx->radio_mutex);

        /* Livrer le paquet hors mutex. */
        if (pkt_len > 0 && ctx->rx_cb) {
            ctx->rx_cb(pkt_buf, (size_t)pkt_len, rssi, ctx->rx_user_ctx);
        }
    }

    ESP_LOGI(TAG, "Tache RX arretee");
    vTaskDelete(NULL);
}

/* ================================================================
 * Implementation de la vtable hal_lora_t
 * ================================================================ */

static hal_err_t c1262_init(const hal_lora_config_t *config, void *ctx_ptr)
{
    core1262_ctx_t *ctx = (core1262_ctx_t *)ctx_ptr;

    if (ctx->initialized) {
        return HAL_OK;
    }
    if (!config) {
        return HAL_ERR_INVALID;
    }

    /* 1. Traduire la config HAL en parametres SX1262. */
    if (!core1262_map_config(config, &ctx->params)) {
        ESP_LOGE(TAG, "Config radio invalide");
        return HAL_ERR_INVALID;
    }

    /* 2. GPIO de controle : NSS, RESET, RXEN, TXEN en sortie ; BUSY,
     *    DIO1 en entree. */
    gpio_config_t out_cfg = {
        .pin_bit_mask = (1ULL << ctx->hw.pin_nss)  |
                        (1ULL << ctx->hw.pin_reset) |
                        (1ULL << ctx->pin_rxen)     |
                        (1ULL << ctx->pin_txen),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    if (gpio_config(&out_cfg) != ESP_OK) return HAL_ERR_IO;
    gpio_set_level(ctx->hw.pin_nss, 1);   /* CS au repos = haut */

    gpio_config_t busy_cfg = {
        .pin_bit_mask = (1ULL << ctx->hw.pin_busy),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    if (gpio_config(&busy_cfg) != ESP_OK) return HAL_ERR_IO;

    gpio_config_t dio1_cfg = {
        .pin_bit_mask = (1ULL << ctx->pin_dio1),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_POSEDGE,   /* IRQ SX1262 = front montant */
    };
    if (gpio_config(&dio1_cfg) != ESP_OK) return HAL_ERR_IO;

    /* 3. Bus + device SPI. CS gere a la main (spics_io_num = -1). */
    spi_bus_config_t bus_cfg = {
        .mosi_io_num     = ctx->pin_mosi,
        .miso_io_num     = ctx->pin_miso,
        .sclk_io_num     = ctx->pin_sck,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = HAL_LORA_MAX_PACKET_SIZE + 16,
    };
    esp_err_t err = spi_bus_initialize(ctx->spi_host, &bus_cfg, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_initialize echoue: %d", err);
        return HAL_ERR_IO;
    }

    spi_device_interface_config_t dev_cfg = {
        .clock_speed_hz = C1262_SPI_CLOCK_HZ,
        .mode           = 0,            /* SX1262 : SPI mode 0 */
        .spics_io_num   = -1,           /* CS pilote a la main dans la glue */
        .queue_size     = 1,
    };
    err = spi_bus_add_device(ctx->spi_host, &dev_cfg, &ctx->hw.spi);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_add_device echoue: %d", err);
        spi_bus_free(ctx->spi_host);
        return HAL_ERR_IO;
    }

    /* 4. Reset materiel du SX1262. */
    if (sx126x_hal_reset(&ctx->hw) != SX126X_HAL_STATUS_OK) {
        ESP_LOGE(TAG, "Reset SX1262 echoue (BUSY bloque ?)");
        return HAL_ERR_IO;
    }

    /* 5. Sequence de configuration radio. */
    hal_err_t rc = apply_radio_config(ctx);
    if (rc != HAL_OK) {
        ESP_LOGE(TAG, "Configuration radio echouee");
        return rc;
    }

    /* 6. Mutex + semaphore + ISR DIO1. */
    ctx->radio_mutex = xSemaphoreCreateMutex();
    ctx->dio1_sem    = xSemaphoreCreateBinary();
    if (!ctx->radio_mutex || !ctx->dio1_sem) {
        ESP_LOGE(TAG, "Creation des primitives FreeRTOS echouee");
        return HAL_ERR_NO_MEM;
    }

    /* gpio_install_isr_service peut deja avoir ete appele par un autre
     * driver (ex: tactile). ESP_ERR_INVALID_STATE = deja installe, OK. */
    err = gpio_install_isr_service(0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "gpio_install_isr_service echoue: %d", err);
        return HAL_ERR_IO;
    }
    err = gpio_isr_handler_add(ctx->pin_dio1, dio1_isr_handler, ctx);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "gpio_isr_handler_add echoue: %d", err);
        return HAL_ERR_IO;
    }

    /* Switch RF au repos en reception. */
    rf_switch_rx(ctx);

    ctx->initialized = true;
    ctx->rx_running  = false;
    ESP_LOGI(TAG, "Core1262 initialise (%lu Hz, SF%u, %d dBm)",
             (unsigned long)ctx->params.freq_hz,
             (unsigned)ctx->params.mod.sf,
             (int)ctx->params.power_dbm);
    return HAL_OK;
}

static hal_err_t c1262_send(const uint8_t *data, size_t len, void *ctx_ptr)
{
    core1262_ctx_t *ctx = (core1262_ctx_t *)ctx_ptr;

    if (!ctx->initialized)              return HAL_FAIL;
    if (!data || len == 0)             return HAL_ERR_INVALID;
    if (len > HAL_LORA_MAX_PACKET_SIZE) return HAL_ERR_INVALID;

    if (xSemaphoreTake(ctx->radio_mutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
        ESP_LOGE(TAG, "send : mutex radio indisponible");
        return HAL_ERR_BUSY;
    }

    const void *sx = &ctx->hw;
    hal_err_t   result = HAL_OK;

    /* 1. Ecrire la charge utile dans la FIFO. */
    if (sx126x_write_buffer(sx, 0x00, data, (uint8_t)len) != SX126X_STATUS_OK) {
        result = HAL_ERR_IO;
        goto done;
    }

    /* 2. Renseigner la taille du paquet dans les parametres LoRa. */
    ctx->params.pkt.pld_len_in_bytes = (uint8_t)len;
    if (sx126x_set_lora_pkt_params(sx, &ctx->params.pkt) != SX126X_STATUS_OK) {
        result = HAL_ERR_IO;
        goto done;
    }

    /* 3. Effacer les IRQ residuelles, basculer le switch en TX, emettre. */
    sx126x_clear_irq_status(sx, SX126X_IRQ_TX_DONE | SX126X_IRQ_TIMEOUT);
    rf_switch_tx(ctx);
    if (sx126x_set_tx(sx, 0) != SX126X_STATUS_OK) {  /* 0 = pas de timeout interne */
        result = HAL_ERR_IO;
        goto done;
    }

    /* 4. Attendre TX_DONE en scrutant le registre IRQ. */
    int waited_ms = 0;
    bool tx_done  = false;
    while (waited_ms < C1262_TX_TIMEOUT_MS) {
        sx126x_irq_mask_t irq = 0;
        if (sx126x_get_irq_status(sx, &irq) != SX126X_STATUS_OK) {
            result = HAL_ERR_IO;
            goto done;
        }
        if (irq & SX126X_IRQ_TX_DONE) { tx_done = true; break; }
        if (irq & SX126X_IRQ_TIMEOUT) { break; }
        vTaskDelay(pdMS_TO_TICKS(5));
        waited_ms += 5;
    }
    sx126x_clear_irq_status(sx, SX126X_IRQ_TX_DONE | SX126X_IRQ_TIMEOUT);

    if (!tx_done) {
        ESP_LOGW(TAG, "send : TX_DONE non recu (%d octets)", (int)len);
        result = HAL_ERR_TIMEOUT;
    }

done:
    /* Toujours rebasculer en reception continue si la tache RX tourne. */
    rf_switch_rx(ctx);
    if (ctx->rx_running) {
        sx126x_set_rx(sx, 0);
    }
    xSemaphoreGive(ctx->radio_mutex);
    return result;
}

static hal_err_t c1262_set_rx_callback(hal_lora_rx_cb_t cb, void *user_ctx,
                                       void *ctx_ptr)
{
    core1262_ctx_t *ctx = (core1262_ctx_t *)ctx_ptr;
    ctx->rx_cb       = cb;
    ctx->rx_user_ctx = user_ctx;
    return HAL_OK;
}

static hal_err_t c1262_start_rx(void *ctx_ptr)
{
    core1262_ctx_t *ctx = (core1262_ctx_t *)ctx_ptr;

    if (!ctx->initialized) return HAL_FAIL;
    if (!ctx->rx_cb)       return HAL_ERR_INVALID;
    if (ctx->rx_running)   return HAL_OK;

    ctx->rx_running = true;

    BaseType_t ok = xTaskCreate(c1262_rx_task, "c1262_rx", C1262_RX_TASK_STACK,
                                ctx, C1262_RX_TASK_PRIO, &ctx->rx_task_handle);
    if (ok != pdPASS) {
        ctx->rx_running = false;
        ESP_LOGE(TAG, "Creation tache RX echouee");
        return HAL_ERR_NO_MEM;
    }

    /* Lancer la reception continue. */
    if (xSemaphoreTake(ctx->radio_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
        rf_switch_rx(ctx);
        sx126x_set_rx(&ctx->hw, 0);
        xSemaphoreGive(ctx->radio_mutex);
    }

    ESP_LOGI(TAG, "Mode reception active");
    return HAL_OK;
}

static hal_err_t c1262_sleep(void *ctx_ptr)
{
    core1262_ctx_t *ctx = (core1262_ctx_t *)ctx_ptr;

    if (!ctx->initialized) return HAL_OK;

    /* Arreter la tache RX. */
    if (ctx->rx_running) {
        ctx->rx_running = false;
        vTaskDelay(pdMS_TO_TICKS(600));   /* laisser la tache sortir de sa boucle */
    }

    /* Mettre le SX1262 en standby basse conso. */
    if (xSemaphoreTake(ctx->radio_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
        sx126x_set_standby(&ctx->hw, SX126X_STANDBY_CFG_RC);
        xSemaphoreGive(ctx->radio_mutex);
    }

    ESP_LOGI(TAG, "Core1262 en veille");
    return HAL_OK;
}

/* ================================================================
 * Factory
 * ================================================================ */

hal_err_t hal_lora_core1262_create(hal_lora_t *lora,
                                   const hal_lora_core1262_pins_t *pins)
{
    if (!lora || !pins) {
        return HAL_ERR_INVALID;
    }

    memset(&s_ctx, 0, sizeof(s_ctx));
    s_ctx.hw.pin_nss   = pins->pin_nss;
    s_ctx.hw.pin_busy  = pins->pin_busy;
    s_ctx.hw.pin_reset = pins->pin_reset;
    s_ctx.pin_dio1     = pins->pin_dio1;
    s_ctx.pin_rxen     = pins->pin_rxen;
    s_ctx.pin_txen     = pins->pin_txen;
    s_ctx.spi_host     = pins->spi_host;
    s_ctx.pin_sck      = pins->pin_sck;
    s_ctx.pin_mosi     = pins->pin_mosi;
    s_ctx.pin_miso     = pins->pin_miso;

    lora->init            = c1262_init;
    lora->send            = c1262_send;
    lora->set_rx_callback = c1262_set_rx_callback;
    lora->start_rx        = c1262_start_rx;
    lora->sleep           = c1262_sleep;
    lora->ctx             = &s_ctx;

    ESP_LOGI(TAG, "Driver cree (SPI%d, NSS=%d, DIO1=%d)",
             pins->spi_host, pins->pin_nss, pins->pin_dio1);
    return HAL_OK;
}
```

> **Correction à appliquer en écrivant le fichier :** la ligne `#include "hal_lora_core1262_internal_unused.h"` ci-dessus est un artefact — **ne pas l'inclure**. Le fichier n'a besoin que des includes réellement listés en dessous (`sx126x_hal_context.h`, `core1262_params.h`, `sx126x.h`, headers ESP-IDF, `string.h`).

- [ ] **Step 3: Vérifier que la branche `CORE1262` du CMake liste bien tous les fichiers**

Vérifier dans `components/device_hal/CMakeLists.txt` que la branche `elseif(CONFIG_MESHPAY_LORA_DRIVER_CORE1262)` (posée en Task 2) contient :
```cmake
    file(GLOB SX126X_SRCS "src/esp32/sx126x/*.c")
    list(APPEND HAL_SRCS
        ${SX126X_SRCS}
        "src/esp32/sx126x_hal.c"
        "src/esp32/core1262_params.c"
        "src/esp32/hal_lora_core1262.c"
    )
```

- [ ] **Step 4: Build de vérification avec le driver Core1262**

Sélectionner temporairement le driver Core1262 :
```bash
idf.py set-target esp32s3
idf.py menuconfig    # Mesh Pay - LoRa -> Driver radio LoRa -> Waveshare Core1262
idf.py build
```
Expected: build OK. Le firmware ESP32-S3 compile avec le backend Core1262, le pilote Semtech vendoré, la glue et le mapper.

Revérifier que le driver Wio-E5 compile toujours :
```bash
idf.py menuconfig    # repasser sur Grove Wio-E5
idf.py build
```
Expected: build OK.

- [ ] **Step 5: Commit**

```bash
git add components/device_hal/src/esp32/hal_lora_core1262.h \
        components/device_hal/src/esp32/hal_lora_core1262.c \
        components/device_hal/CMakeLists.txt
git commit -m "feat(device_hal): backend HAL LoRa Core1262 (SX1262 SPI)

Remplit la vtable hal_lora_t : bring-up radio via le pilote Semtech,
send bloquant (scrutation TX_DONE), tache RX sur IRQ DIO1, switch RF
pilote par RXEN/TXEN. Le bring-up RF reste a valider sur materiel.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>"
```

---

## Task 8: Basculer les défauts sur Core1262

**Files:**
- Modify: `components/device_hal/Kconfig`
- Modify: `sdkconfig.defaults.esp32`
- Modify: `sdkconfig.defaults.esp32s3`

Le Core1262 devient le driver par défaut. L'ESP32 (CYD) garde explicitement le Wio-E5 via son `sdkconfig.defaults`.

- [ ] **Step 1: Changer le défaut du `choice` Kconfig**

Modify `components/device_hal/Kconfig`. Dans le bloc `choice MESHPAY_LORA_DRIVER`, remplacer :

```
    default MESHPAY_LORA_DRIVER_WIO_E5
```

par :

```
    default MESHPAY_LORA_DRIVER_CORE1262
```

- [ ] **Step 2: Figer le driver par carte dans les `sdkconfig.defaults`**

Modify `sdkconfig.defaults.esp32` — ajouter à la fin :

```
# --- LoRa : la carte CYD (ESP32) est equipee du module Grove Wio-E5 ---
CONFIG_MESHPAY_LORA_DRIVER_WIO_E5=y
```

Modify `sdkconfig.defaults.esp32s3` — ajouter à la fin :

```
# --- LoRa : la carte Waveshare (ESP32-S3) est equipee du module Core1262 ---
CONFIG_MESHPAY_LORA_DRIVER_CORE1262=y
```

- [ ] **Step 3: Build de vérification des deux cibles avec leurs drivers respectifs**

Run: `idf.py fullclean && idf.py set-target esp32 && idf.py build`
Expected: build OK. `idf.py menuconfig` confirmerait `Driver radio LoRa = Grove Wio-E5` (depuis `sdkconfig.defaults.esp32`).

Run: `idf.py fullclean && idf.py set-target esp32s3 && idf.py build`
Expected: build OK. `Driver radio LoRa = Waveshare Core1262` (depuis `sdkconfig.defaults.esp32s3`).

- [ ] **Step 4: Build de vérification de la test app**

Run: `idf.py -C test_app fullclean && idf.py -C test_app set-target esp32s3 && idf.py -C test_app build`
Expected: build OK. La test app hérite du défaut `CORE1262` → `test_core1262_params.c` est compilé.

- [ ] **Step 5: Commit**

```bash
git add components/device_hal/Kconfig \
        sdkconfig.defaults.esp32 sdkconfig.defaults.esp32s3
git commit -m "feat(device_hal): Core1262 par defaut, Wio-E5 fige sur la carte ESP32

Le defaut Kconfig passe a Core1262. sdkconfig.defaults.esp32 garde
explicitement le Wio-E5 (carte CYD), sdkconfig.defaults.esp32s3 prend
le Core1262 (carte Waveshare).

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>"
```

---

## Task 9: Smoke test matériel

**Files:** aucun (procédure de validation sur hardware)

Cette tâche valide le chemin RF que les builds ne peuvent pas couvrir. Prérequis : un module Core1262 câblé sur une carte Waveshare ESP32-S3 selon le brochage Kconfig (SCK=IO1, MOSI=IO2, MISO=IO3, NSS=IO4, RESET=IO5, BUSY=IO6, DIO1=IO7, RXEN=IO8, TXEN=IO9, 3V3 sur VCC3V3, GND), antenne 868 MHz sur ANT.

- [ ] **Step 1: Flasher le firmware Core1262 sur l'ESP32-S3**

Run: `idf.py set-target esp32s3 && idf.py build && idf.py -p <PORT> flash monitor`
Expected au boot, dans les logs série :
- `hal_lora_c1262: Driver cree (SPI2, NSS=4, DIO1=7)`
- `hal_lora_c1262: Core1262 initialise (868100000 Hz, SF9, 14 dBm)`
- `hal_lora_c1262: Tache RX demarree`

> Si `Reset SX1262 echoue (BUSY bloque ?)` → vérifier le câblage BUSY/RESET/3V3.
> Si `Configuration radio echouee` → suspecter la tension TCXO : tester `SX126X_TCXO_CTRL_1_7V` ou `SX126X_TCXO_CTRL_3_3V` dans `C1262_TCXO_VOLTAGE` (`hal_lora_core1262.c`).

- [ ] **Step 2: Test d'émission/réception entre deux devices**

Avec un 2ᵉ device LoRa (un autre Core1262, ou la carte CYD en Wio-E5 — même fréquence/SF/BW/CR), provoquer une émission depuis l'un et vérifier la réception sur l'autre via les logs `lora_sync` / le moniteur multi-device (cf. doctech « 11 - Moniteur multi-device série »).
Expected : un paquet émis d'un côté apparaît côté récepteur avec un RSSI plausible (ex. −30 à −90 dBm à courte distance).

- [ ] **Step 3: Consigner le résultat**

Noter dans le doctech (« 08 - Journal des corrections récentes » ou un smoke test dédié) : tension TCXO retenue, config PA validée, RSSI observé, portée constatée. Si des valeurs de `hal_lora_core1262.c` ont dû être ajustées, faire un commit `fix(hal_lora_c1262): ...` correspondant.

---

## Self-Review

**1. Couverture de la spec** (`Doctech/14 - Driver LoRa Core1262 (design).md`) :
- §2 D1 (Kconfig) → Task 1 ✅
- §2 D2 (LoRa obligatoire, stub supprimé) → Task 3 ✅
- §2 D3 (pilote Semtech vendoré + glue) → Tasks 4, 6 ✅
- §2 D4 (factory unifiée) → Task 2 ✅
- §3 (aucun changement applicatif) → respecté : seuls `transport_lora.c` (façade) + CMake sont touchés côté `main/` ✅
- §5 (fichiers créés) → Tasks 1-7 ✅
- §6 (Kconfig pins) → Task 1 ✅
- §7 (CMake, transport, stub, sdkconfig.defaults) → Tasks 2, 3, 8 ✅
- §8 (config radio inchangée dans lora_sync, câblage 9 GPIO) → respecté : `lora_sync.c` non modifié ; pins Kconfig conformes au câblage vérifié ✅
- §9 (tests : mapper + smoke test) → Tasks 5, 9 ✅
- §10 (séquence d'implémentation) → l'ordre des tasks suit le phasage (Kconfig → factory/refactor pur → vendoring → mapper TDD → glue → backend → bascule défauts → smoke test) ✅

**2. Placeholders** : un artefact `#include "hal_lora_core1262_internal_unused.h"` a été laissé volontairement dans le code de Task 7 Step 2 avec une consigne explicite de ne pas l'inclure — c'est une protection anti-copie-aveugle, pas un placeholder oublié. Aucun « TODO », aucune section vide.

**3. Cohérence des types** : `hal_lora_core1262_pins_t` (Task 7) ↔ champs lus en Kconfig (Task 1) ↔ initialisation dans `hal_lora_factory.c` (Task 2) — vérifié champ par champ. `core1262_hw_t` (Task 6) utilisé par la glue et par le backend de façon cohérente. `core1262_radio_params_t` (Task 5) consommé par `apply_radio_config()` / `c1262_send()` (Task 7) — champs `mod`, `pkt`, `freq_hz`, `power_dbm` cohérents. Fonctions Semtech (`sx126x_*`) conformes aux signatures du driver vendoré.
