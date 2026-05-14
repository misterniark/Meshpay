# Portage LoRa Wio-E5 sur ESP32-S3 (Waveshare 1.47") — Design

**Date :** 2026-05-14
**Statut :** validé, prêt pour implémentation

## 1. Objectif

Activer la connectivité LoRa sur la cible ESP32-S3 (carte Waveshare
ESP32-S3-Touch-LCD-1.47) en réutilisant **à l'identique** le driver
`hal_lora_wio_e5.c` et la façade `transport_lora.c` déjà en place sur le
CYD (ESP32). Aujourd'hui le S3 lie le stub no-op `transport_lora_stub.c`
et ne compile pas le driver Wio-E5.

Après ce portage, le S3 devient un **nœud LoRa complet**, identique au
CYD : sync DAG périodique + relais multi-hop des broadcasts texte et des
PING maître. Le comportement reste piloté au runtime par le rôle du
device (`is_master`), pas par la cible.

## 2. Décisions de cadrage

| Question | Décision |
|---|---|
| Rôle LoRa du S3 | Nœud complet — réutilisation intégrale de `transport_lora.c` |
| Relais multi-hop sur S3 | Actif, comme le CYD (cohérence de flotte + portée texte/PING) |
| Pins UART | GPIO 43 (TX) / GPIO 44 (RX), UART1 |
| Approche de gating | Étendre le gate de cible (`... OR CONFIG_IDF_TARGET_ESP32S3`) |

Note : la sync DAG est intrinsèquement 1-hop *par transmission* mais
converge multi-hop par gossip (chaque device re-broadcaste son propre DAG
toutes les 2 min). Le relais multi-hop ne concerne **que** les broadcasts
texte et les PING maître — il ne transporte pas le trafic de sync DAG.

## 3. Câblage matériel

Module Grove Seeed Wio-E5 (STM32WLE5JC), interface UART/AT à 9600 bauds
(défaut usine, déjà codé dans le driver).

| S3 (Waveshare 1.47") | Wio-E5 | Note |
|---|---|---|
| GPIO 43 (U0TXD) | RX | UART1 routé via matrice GPIO |
| GPIO 44 (U0RXD) | TX | |
| 3V3 | VCC | |
| GND | GND | |

GPIO 43/44 sont les broches sérigraphiées TX/RX du header d'extension.
Elles sont libres car la console série du S3 passe par l'USB-Serial-JTAG
(USB-C natif, GPIO 19/20). Source de vérité du pinout : la définition de
carte CircuitPython officielle (`waveshare_esp32_s3_touch_lcd_1_47`).

GPIO occupés sur la carte (pour mémoire) : LCD 38/39/21/45/40/46,
tactile 41/42/47/48, MicroSD 13–18, USB 19/20, flash+PSRAM octal 26–37.
GPIO libres sur le header : 1–11 et 43/44.

## 4. Changements

Approche A — gate de cible étendu. Aucun changement fonctionnel dans
`main.c` (la factory LoRa est déjà `extern` dans `transport_lora.c`, et
`transport_lora_init_and_start()` est déjà appelé inconditionnellement).

### 4.1 `components/device_hal/CMakeLists.txt`

Ajouter `"src/esp32/hal_lora_wio_e5.c"` à la branche
`elseif(CONFIG_IDF_TARGET_ESP32S3)` (le `src/esp32` est déjà dans
`HAL_PRIV_INCLUDE` pour cette branche, et le fichier est target-agnostic :
UART/AT standard). Mettre à jour le commentaire d'en-tête ligne 5
(« JD9853 display, NVS storage (client-only, pas de LoRa) »).

### 4.2 `main/CMakeLists.txt`

Changer le gate de sélection `transport_lora.c` :

```cmake
if(CONFIG_IDF_TARGET_ESP32 OR CONFIG_IDF_TARGET_ESP32S3)
    list(APPEND meshpay_main_srcs "transport/transport_lora.c")
else()
    list(APPEND meshpay_main_srcs "transport/transport_lora_stub.c")
endif()
```

Mettre à jour les commentaires lignes 4–5 et 58–60.

### 4.3 `main/transport/transport_lora.c`

Rendre les pins conditionnels par cible :

```c
/* Pins LoRa Wio-E5 — selon la cible matérielle. */
#if CONFIG_IDF_TARGET_ESP32
/* CYD (ESP32-2432S028) : UART2, header libre. */
#define LORA_UART_NUM    2
#define LORA_TX_PIN     17
#define LORA_RX_PIN     16
#elif CONFIG_IDF_TARGET_ESP32S3
/* Waveshare ESP32-S3-Touch-LCD-1.47 : UART1 sur GPIO 43/44 (broches
 * U0 du header, libres car console sur USB-Serial-JTAG). */
#define LORA_UART_NUM    1
#define LORA_TX_PIN     43
#define LORA_RX_PIN     44
#endif
```

Mettre à jour le commentaire d'en-tête du fichier (lignes 3–6 :
« cible ESP32 CYD », « Compile uniquement quand CONFIG_IDF_TARGET_ESP32 »).

### 4.4 `sdkconfig.defaults.esp32s3`

Ajouter `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y`.

Aujourd'hui ce réglage n'est présent que dans `test_app/sdkconfig.defaults`,
pas dans les defaults du firmware principal. Sans lui, le build S3 du
firmware principal risque de défaulter la console sur UART0 (GPIO 43/44),
ce qui entrerait en conflit direct avec le câblage Wio-E5. C'est un point
de correction critique.

### 4.5 `main/app_state.h`

Étendre le gate de la capability `MP_HAS_LORA` pour inclure
`CONFIG_IDF_TARGET_ESP32S3`, et corriger le commentaire ligne 55
(« sur la carte CYD (ESP32 uniquement) »). `MP_HAS_LORA` n'est plus
référencé que dans des commentaires depuis le Lot D.3 — la mise à jour
est faite pour cohérence documentaire.

### 4.6 Nettoyage des commentaires devenus faux

In-scope car ils deviendraient activement mensongers après le changement :

- `main/transport/transport_lora_stub.c` : en-tête (« cibles sans Wio-E5,
  ex: ESP32-S3 »).
- `main/transport/transport_lora.h` : lignes 8–16 (« Sur ESP32-S3 (pas de
  Wio-E5 onboard) »).
- `main/main.c` : commentaire lignes ~316–318 (« LoRa : uniquement sur les
  cartes qui embarquent un Wio-E5 ») — à reformuler, les deux cibles en ont.
- `main/ops/op_master.c` : lignes 9–11 (« Sur cibles sans LoRa (ESP32-S3),
  transport_lora_send est no-op »).
- `main/ops/ops.h` : lignes 45–46 (même formulation).

Périmètre strictement limité aux commentaires rendus faux par ce
changement — pas de refactor non lié.

### 4.7 `components/debug_console/src/debug_console.c` (bug découvert au build)

Le changement 4.4 (console sur USB-Serial-JTAG) active dans
`console_io_init()` le bloc `#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG`,
jusque-là jamais compilé sur le build S3 principal (console par défaut
sur UART). Ce bloc contenait un bug latent : il était resté sur l'API
dépréciée `esp_vfs_usb_serial_jtag_use_driver()` avec le header
`esp_vfs_usb_serial_jtag.h` qui ne déclare pas cette fonction (le
symbole déprécié vit dans `esp_vfs_dev.h`). La branche UART avait été
migrée vers la nomenclature `*_vfs_*` d'IDF v5.x, mais pas la branche
USB-Serial-JTAG.

Correctif (miroir exact de la branche UART) :
- include : `esp_vfs_usb_serial_jtag.h` → `driver/usb_serial_jtag_vfs.h`
- appel : `esp_vfs_usb_serial_jtag_use_driver()` →
  `usb_serial_jtag_vfs_use_driver()`

In-scope : le changement 4.4 est nécessaire et correct ; il faut que le
firmware S3 compile pour que le portage LoRa puisse être livré.

## 5. Flux d'exécution (inchangé, hérité du CYD)

```
app_main()
  └─ transport_lora_init_and_start()
       ├─ hal_lora_wio_e5_create(&s_lora_hal, UART1, 43, 44)
       │    └─ uart_param_config + uart_set_pin + uart_driver_install
       └─ xTaskCreate(lora_sync_task, ...)
            ├─ toutes les 2 min : lora_collect_confirmed_txs() → broadcast
            └─ RX : LORA_TX → COMM_EVT_LORA_TX_RECEIVED → core_task merge DAG

core_task (hors mutex)
  └─ transport_lora_pump()  ← relais broadcast/ping + PONG différé
```

## 6. Gestion d'erreur (inchangée)

Si `hal_lora_wio_e5_create` échoue (Wio-E5 absent ou non câblé),
`transport_lora_init_and_start` logge un WARN et retourne l'erreur ; le
firmware continue sans LoRa. Pas de boot loop, pas de blocage. Le code
appelant (`main.c:332`) ignore volontairement la valeur de retour
(`(void)transport_lora_init_and_start();`).

## 7. Vérification

Aucune nouvelle fonction C n'est créée — uniquement du gating de build et
des constantes compile-time. Il n'y a donc pas de test unitaire à écrire ;
la vérification est :

1. **Build S3** (`idf.py set-target esp32s3 && idf.py build`) → ✅ OK,
   `offline-payment.bin` généré (33 % de flash libre).
2. **Non-régression CYD** (`idf.py set-target esp32 && idf.py build`) →
   ✅ OK, `offline-payment.bin` généré, 0 erreur.
3. **Build de test** (`idf.py -C test_app build`) → ✅ OK,
   `meshpay_test_app.bin` généré. `hal_lora_wio_e5.c` et `hal_lora_mock.c`
   coexistent sans conflit de symbole (factory `hal_lora_wio_e5_create`
   ≠ `hal_lora_mock_create`).
4. **Smoke test matériel** (Wio-E5 câblé sur GPIO 43/44) : flasher le S3,
   vérifier dans le log série :
   - `Driver cree (UART1, TX=43, RX=44)`
   - `Wio-E5 initialise (UART1)`
   - `[11/12] HAL initialises (ESP-NOW + LoRa)`
5. **Fonctionnel** : observer une émission de sync au bout de ~2 min et la
   réception d'une TX émise par un autre nœud (CYD ou S3).

Les étapes 4–5 nécessitent le matériel et ne peuvent pas être automatisées.

## 8. Risques et points d'attention

- **Console UART0 vs USB-Serial-JTAG** : couvert par le changement 4.4.
  Si un build S3 antérieur a un `sdkconfig` généré avec la console sur
  UART0, il faudra le régénérer (`rm sdkconfig`) ou passer par
  `menuconfig` pour récupérer le nouveau defaults.
- **Bruit au boot sur GPIO 43** : avant que le firmware ne configure
  UART1, GPIO 43 (U0TXD) peut émettre brièvement. Le Wio-E5 ignore toute
  entrée AT malformée — non bloquant.
- **Airtime LoRa** : le relais multi-hop actif sur le S3 ajoute du trafic,
  mais le délai aléatoire anti-collision et le cache `MAX_SEEN_BROADCASTS`
  sont déjà en place (hérités du CYD). Non-significatif pour une flotte de
  quelques devices.
