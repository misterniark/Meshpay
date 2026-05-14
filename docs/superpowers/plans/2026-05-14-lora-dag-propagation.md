# Propagation DAG via LoRa — fix fragmentation + corrections doctech — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Câbler la fragmentation LoRa à l'émission (les TX dont le CBOR dépasse 254 octets ne sont aujourd'hui jamais propagées), puis corriger la documentation technique Obsidian pour refléter la réalité et tracer la fragilité résiduelle du gossip.

**Architecture :** Un nouveau module *pur* `lora_tx_packetize` (sans dépendance FreeRTOS/HAL, testable en natif) transforme une `transaction_t` en 1 paquet `LORA_TX` direct (si le CBOR tient dans 255 octets) ou N fragments `LORA_FRAG` via le `lora_frag_split()` existant. `lora_sync_do_cycle()` l'appelle au lieu d'empaqueter à la main dans un buffer de 255 octets et d'abandonner silencieusement les grosses TX. Les corrections doc suivent, dans le vault Obsidian.

**Tech Stack :** C (ESP-IDF), CMake, Unity (test_app embarqué), Obsidian Markdown.

---

## Contexte — pourquoi ce plan

Investigation menée le 2026-05-14 sur `components/comm/lora_sync/`. Trois constats :

1. **Bug réel.** `lora_frag_split()` (fragmentation à l'émission) est implémenté et testé (`test_lora_frag.c`) mais **n'est jamais appelé en production**. `lora_sync_do_cycle()` empaquette chaque TX via `comm_msg_pack_lora_tx()` dans un buffer `uint8_t buf[COMM_MSG_LORA_MAX]` (255 octets) ; si la TX sérialisée dépasse 254 octets, `comm_msg_pack_lora_tx()` renvoie -1 et la TX est **abandonnée sans aucun log**. Or `TX_CBOR_MAX_SIZE = 320` : une TX TRANSFER à 2 parents (~282 octets de CBOR) est donc indiffusable en LoRa, alors qu'elle circule sans problème en ESP-NOW V2.
2. **Doc fausse.** La note Doctech `04 - Décisions techniques.md` affirme « Taille d'une TX sérialisée : ~233 octets (sous 250 ✅) » et « contrainte 250 octets ESP-NOW » — obsolète depuis le passage à ESP-NOW V2 / `TX_CBOR_MAX_SIZE = 320` (Lot E.1bis).
3. **Fragilité non tracée.** La convergence du DAG via LoRa repose uniquement sur la re-diffusion périodique pilotée par un filigrane temporel unique (`last_sync_ts`), sans relais explicite ni anti-entropie, avec une troncature silencieuse à 32 TX/cycle. Aucune entrée de dette technique ne le documente.

Ce plan traite les trois : Tasks 1-2 = le code, Tasks 3-5 = la doc.

---

## File Structure

| Fichier | Action | Responsabilité |
|---|---|---|
| `components/comm/lora_sync/include/comm/lora_tx_packetize.h` | Créer | API publique du module pur : `lora_tx_packetize()` |
| `components/comm/lora_sync/src/lora_tx_packetize.c` | Créer | Implémentation : sérialise la TX, choisit paquet direct ou fragments |
| `components/comm/lora_sync/test/test_lora_tx_packetize.c` | Créer | 4 TEST_CASE Unity : paquet unique, fragmentation, round-trip, args NULL |
| `components/comm/lora_sync/CMakeLists.txt` | Modifier | Enregistrer le nouveau `.c` (firmware + test_app) et le `.c` de test |
| `components/comm/lora_sync/src/lora_sync.c` | Modifier | Remplacer la boucle d'envoi de `lora_sync_do_cycle()` par un helper qui appelle `lora_tx_packetize()` |
| `~/Documents/Obsidian/Misterniark/Projet/Mesh Pay/Doctech/04 - Décisions techniques.md` | Modifier (vault Obsidian) | Corriger la taille des TX et la ligne fragmentation |
| `~/Documents/Obsidian/Misterniark/Projet/Mesh Pay/Doctech/07 - Dette technique.md` | Modifier (vault Obsidian) | Ajouter l'entrée de dette « Gossip LoRa fragile » + ligne de résumé |
| `~/Documents/Obsidian/Misterniark/Projet/Mesh Pay/Doctech/08 - Journal des corrections récentes.md` | Modifier (vault Obsidian) | Ajouter l'entrée de journal du fix |

> **Note dépôt :** le vault Obsidian (`~/Documents/Obsidian/...`) est **séparé du dépôt firmware**. Les Tasks 3-5 éditent des fichiers hors du worktree git : pas de `git add`/`git commit` pour ces tâches, juste l'édition.

> **Invariant de format LoRa (à respecter dans Task 1) :** le récepteur (`lora_sync.c`) traite les deux types différemment.
> - `LORA_TX` direct : le paquet est `[COMM_MSG_LORA_TX][cbor...]`, le récepteur appelle `comm_msg_unpack_lora_tx()` sur le **tout** (byte de type inclus).
> - `LORA_FRAG` : après réassemblage, le récepteur appelle `tx_deserialize()` directement sur le buffer concaténé → les fragments doivent contenir le **CBOR nu, sans byte de type**.
> `lora_tx_packetize()` doit donc préfixer `COMM_MSG_LORA_TX` dans le cas direct, mais fragmenter le CBOR **brut** dans le cas fragmenté.

---

## Task 1 : Module pur `lora_tx_packetize`

**Files:**
- Create: `components/comm/lora_sync/include/comm/lora_tx_packetize.h`
- Create: `components/comm/lora_sync/src/lora_tx_packetize.c`
- Create: `components/comm/lora_sync/test/test_lora_tx_packetize.c`
- Modify: `components/comm/lora_sync/CMakeLists.txt`

- [ ] **Step 1 : Écrire le fichier de test**

Créer `components/comm/lora_sync/test/test_lora_tx_packetize.c` :

```c
/**
 * @file test_lora_tx_packetize.c
 * @brief Tests unitaires de lora_tx_packetize() — emballage d'une
 *        transaction confirmée en paquet(s) LoRa, direct ou fragmenté.
 *
 * Ne définit volontairement PAS setUp()/tearDown() : test_lora_frag.c
 * fournit déjà des versions weak partagées par le composant.
 */

#include "unity.h"
#include "comm/lora_tx_packetize.h"
#include "comm/comm_msg.h"   /* COMM_MSG_LORA_TX, COMM_MSG_LORA_FRAG, COMM_MSG_LORA_MAX */
#include "comm/lora_frag.h"
#include "transaction/tx_serialize.h"
#include "transaction/tx_types.h"
#include "esp_err.h"
#include <string.h>

/*
 * Petite TX : 1 parent, tous les champs à zéro. Son CBOR (~225 octets)
 * tient dans un seul paquet LoRa.
 */
static void build_small_tx(transaction_t *tx)
{
    memset(tx, 0, sizeof(*tx));
    tx->type         = TX_TYPE_TRANSFER;
    tx->parent_count = 1;
    tx->status       = TX_STATUS_CONFIRMED;
}

/*
 * Grosse TX : 2 parents, tous les champs binaires remplis de 0xFF et les
 * entiers à leur max. Reproduit le pire cas réel (TRANSFER à 2 parents),
 * dont le CBOR (~282 octets) dépasse COMM_MSG_LORA_MAX et doit donc être
 * fragmenté. Le memset(0) initial garde les octets de padding de la
 * struct déterministes (à 0) pour permettre un memcmp round-trip fiable.
 */
static void build_large_tx(transaction_t *tx)
{
    memset(tx, 0, sizeof(*tx));
    tx->type         = TX_TYPE_TRANSFER;
    tx->status       = TX_STATUS_CONFIRMED;
    tx->parent_count = 2;
    tx->amount       = 0xFFFFFFFFu;
    tx->currency_id  = 0xFFFFFFFFu;
    tx->fee          = 0xFFFFFFFFu;
    tx->seq          = 0xFFFFFFFFu;
    tx->timestamp    = 0xFFFFFFFFFFFFFFFFull;
    memset(&tx->id,        0xFF, sizeof(tx->id));
    memset(&tx->from,      0xFF, sizeof(tx->from));
    memset(&tx->to,        0xFF, sizeof(tx->to));
    memset(&tx->parents,   0xFF, sizeof(tx->parents));
    memset(&tx->signature, 0xFF, sizeof(tx->signature));
}

TEST_CASE("packetize_small_tx_single_packet", "[lora_tx_packetize]")
{
    transaction_t tx;
    build_small_tx(&tx);

    uint8_t packets[LORA_FRAG_MAX_FRAGMENTS][LORA_FRAG_PACKET_MAX];
    size_t  packet_lens[LORA_FRAG_MAX_FRAGMENTS];
    uint8_t count = 0;

    TEST_ASSERT_EQUAL(0, lora_tx_packetize(&tx, 7, packets, packet_lens, &count));
    TEST_ASSERT_EQUAL(1, count);
    TEST_ASSERT_EQUAL(COMM_MSG_LORA_TX, packets[0][0]);
    TEST_ASSERT_LESS_OR_EQUAL(COMM_MSG_LORA_MAX, packet_lens[0]);
}

TEST_CASE("packetize_large_tx_fragments", "[lora_tx_packetize]")
{
    transaction_t tx;
    build_large_tx(&tx);

    uint8_t packets[LORA_FRAG_MAX_FRAGMENTS][LORA_FRAG_PACKET_MAX];
    size_t  packet_lens[LORA_FRAG_MAX_FRAGMENTS];
    uint8_t count = 0;

    TEST_ASSERT_EQUAL(0, lora_tx_packetize(&tx, 42, packets, packet_lens, &count));
    /*
     * Une TX TRANSFER à 2 parents dépasse 254 octets de CBOR. Si cette
     * assertion échoue, le pire cas tient en fait dans un seul paquet et
     * le bug d'origine n'était pas atteignable — information utile en soi.
     */
    TEST_ASSERT_GREATER_OR_EQUAL(2, count);
    for (uint8_t i = 0; i < count; i++) {
        TEST_ASSERT_EQUAL(COMM_MSG_LORA_FRAG, packets[i][0]); /* type */
        TEST_ASSERT_EQUAL(i, packets[i][1]);                  /* index */
        TEST_ASSERT_EQUAL(count, packets[i][2]);              /* total */
        TEST_ASSERT_EQUAL(42, packets[i][3]);                 /* seq_id */
        TEST_ASSERT_LESS_OR_EQUAL(LORA_FRAG_PACKET_MAX, packet_lens[i]);
    }
}

TEST_CASE("packetize_large_tx_roundtrip", "[lora_tx_packetize]")
{
    transaction_t original;
    build_large_tx(&original);

    uint8_t packets[LORA_FRAG_MAX_FRAGMENTS][LORA_FRAG_PACKET_MAX];
    size_t  packet_lens[LORA_FRAG_MAX_FRAGMENTS];
    uint8_t count = 0;
    TEST_ASSERT_EQUAL(0, lora_tx_packetize(&original, 99, packets, packet_lens, &count));
    TEST_ASSERT_GREATER_OR_EQUAL(2, count);

    /* Réassembler les fragments comme le ferait le récepteur LoRa. */
    lora_frag_ctx_t ctx;
    lora_frag_ctx_init(&ctx);
    bool complete = false;
    for (uint8_t i = 0; i < count; i++) {
        complete = lora_frag_receive(&ctx,
                                     packets[i][1], packets[i][2], packets[i][3],
                                     &packets[i][LORA_FRAG_HEADER_SIZE],
                                     packet_lens[i] - LORA_FRAG_HEADER_SIZE,
                                     0 /* current_time */);
    }
    TEST_ASSERT_TRUE(complete);

    uint8_t result[LORA_FRAG_MAX_FRAGMENTS * LORA_FRAG_PAYLOAD_MAX];
    size_t  result_len = 0;
    TEST_ASSERT_EQUAL(0, lora_frag_get_result(&ctx, result, sizeof(result), &result_len));

    /*
     * Le buffer réassemblé est le CBOR nu : tx_deserialize() doit
     * reconstruire la TX à l'identique. memset(0) garantit que les octets
     * de padding de `restored` matchent ceux d'`original`.
     */
    transaction_t restored;
    memset(&restored, 0, sizeof(restored));
    TEST_ASSERT_EQUAL(ESP_OK, tx_deserialize(result, result_len, &restored));
    TEST_ASSERT_EQUAL_MEMORY(&original, &restored, sizeof(transaction_t));
}

TEST_CASE("packetize_null_args", "[lora_tx_packetize]")
{
    uint8_t packets[LORA_FRAG_MAX_FRAGMENTS][LORA_FRAG_PACKET_MAX];
    size_t  packet_lens[LORA_FRAG_MAX_FRAGMENTS];
    uint8_t count = 0;
    transaction_t tx;
    build_small_tx(&tx);

    TEST_ASSERT_EQUAL(-1, lora_tx_packetize(NULL, 0, packets, packet_lens, &count));
    TEST_ASSERT_EQUAL(-1, lora_tx_packetize(&tx, 0, NULL, packet_lens, &count));
    TEST_ASSERT_EQUAL(-1, lora_tx_packetize(&tx, 0, packets, NULL, &count));
    TEST_ASSERT_EQUAL(-1, lora_tx_packetize(&tx, 0, packets, packet_lens, NULL));
}
```

- [ ] **Step 2 : Écrire le header**

Créer `components/comm/lora_sync/include/comm/lora_tx_packetize.h` :

```c
/**
 * @file lora_tx_packetize.h
 * @brief Emballage d'une transaction confirmée en paquet(s) LoRa.
 *
 * Module pur : aucune dépendance FreeRTOS ni HAL, testable en natif.
 * Pont entre la (dé)sérialisation CBOR (`transaction`) et la
 * fragmentation (`lora_frag`).
 */

#ifndef LORA_TX_PACKETIZE_H
#define LORA_TX_PACKETIZE_H

#include "transaction/tx_types.h"  /* transaction_t */
#include "comm/lora_frag.h"        /* LORA_FRAG_PACKET_MAX, LORA_FRAG_MAX_FRAGMENTS */
#include <stdint.h>
#include <stddef.h>

/**
 * Transforme une transaction en un ou plusieurs paquets LoRa prêts à émettre.
 *
 * - Si la TX sérialisée tient dans un paquet LoRa (1 + cbor <= COMM_MSG_LORA_MAX),
 *   produit 1 paquet direct : [COMM_MSG_LORA_TX][cbor...].
 * - Sinon, fragmente le CBOR *nu* via lora_frag_split() : N paquets
 *   [COMM_MSG_LORA_FRAG][index][total][seq_id][chunk...].
 *
 * L'asymétrie (byte de type présent dans le cas direct, absent des
 * fragments) est imposée par le récepteur : voir lora_sync.c, qui appelle
 * comm_msg_unpack_lora_tx() sur le paquet direct mais tx_deserialize()
 * sur le buffer réassemblé.
 *
 * @param tx            Transaction à émettre
 * @param seq_id        Identifiant de séquence de fragmentation (0-255),
 *                      utilisé uniquement si la TX est fragmentée
 * @param packets       [out] Tableau de paquets (LORA_FRAG_MAX_FRAGMENTS lignes)
 * @param packet_lens   [out] Taille réelle de chaque paquet produit
 * @param packet_count  [out] Nombre de paquets produits (toujours >= 1 en cas de succès)
 * @return 0 en cas de succès,
 *         -1 si paramètre NULL, échec de sérialisation, ou TX trop
 *         volumineuse pour LORA_FRAG_MAX_FRAGMENTS fragments
 */
int lora_tx_packetize(const transaction_t *tx,
                      uint8_t seq_id,
                      uint8_t packets[][LORA_FRAG_PACKET_MAX],
                      size_t  *packet_lens,
                      uint8_t *packet_count);

#endif /* LORA_TX_PACKETIZE_H */
```

- [ ] **Step 3 : Écrire l'implémentation**

Créer `components/comm/lora_sync/src/lora_tx_packetize.c` :

```c
/**
 * @file lora_tx_packetize.c
 * @brief Emballage d'une transaction confirmée en paquet(s) LoRa.
 */

#include "comm/lora_tx_packetize.h"
#include "comm/comm_msg.h"            /* COMM_MSG_LORA_TX, COMM_MSG_LORA_MAX */
#include "comm/lora_frag.h"           /* lora_frag_split() */
#include "transaction/tx_serialize.h" /* tx_serialize_full(), TX_CBOR_MAX_SIZE */
#include "esp_err.h"
#include <string.h>

int lora_tx_packetize(const transaction_t *tx,
                      uint8_t seq_id,
                      uint8_t packets[][LORA_FRAG_PACKET_MAX],
                      size_t  *packet_lens,
                      uint8_t *packet_count)
{
    if (!tx || !packets || !packet_lens || !packet_count) {
        return -1;
    }

    /*
     * Sérialiser la TX complète en CBOR dans un buffer dimensionné au pire
     * cas (TX_CBOR_MAX_SIZE = 320 octets) : tx_serialize_full() ne peut
     * donc pas échouer faute de place pour une TX valide.
     */
    uint8_t cbor[TX_CBOR_MAX_SIZE];
    size_t  cbor_len = 0;
    if (tx_serialize_full(tx, cbor, sizeof(cbor), &cbor_len) != ESP_OK) {
        return -1;
    }

    /*
     * Cas direct : [type:1][cbor...] tient dans un paquet LoRa. Le byte de
     * type est nécessaire ici car le récepteur appelle
     * comm_msg_unpack_lora_tx() sur le paquet entier.
     */
    if (1 + cbor_len <= COMM_MSG_LORA_MAX) {
        packets[0][0] = COMM_MSG_LORA_TX;
        memcpy(&packets[0][1], cbor, cbor_len);
        packet_lens[0] = 1 + cbor_len;
        *packet_count  = 1;
        return 0;
    }

    /*
     * Cas fragmenté : on fragmente le CBOR *nu*, sans byte de type. Le
     * récepteur réassemble puis appelle tx_deserialize() directement sur
     * le buffer concaténé — il n'attend donc aucun préfixe.
     */
    return lora_frag_split(cbor, cbor_len, seq_id,
                           packets, packet_lens, packet_count);
}
```

- [ ] **Step 4 : Enregistrer les nouveaux fichiers dans le CMakeLists du composant**

Modifier `components/comm/lora_sync/CMakeLists.txt`.

Remplacer :

```cmake
set(lora_sync_srcs
    "src/lora_sync.c"
    "src/lora_frag.c"
)
```

par :

```cmake
set(lora_sync_srcs
    "src/lora_sync.c"
    "src/lora_frag.c"
    "src/lora_tx_packetize.c"
)
```

Puis, dans la branche `if(project_name STREQUAL "meshpay_test_app")`, remplacer :

```cmake
    list(APPEND lora_sync_srcs "test/test_lora_frag.c")
```

par :

```cmake
    list(APPEND lora_sync_srcs "test/test_lora_frag.c")
    list(APPEND lora_sync_srcs "test/test_lora_tx_packetize.c")
```

(Aucun changement de `REQUIRES` nécessaire : `comm_protocol` et `transaction` y sont déjà.)

- [ ] **Step 5 : Compiler le test_app (porte de compilation — toujours exécutable)**

Run: `idf.py -C test_app build`
Expected: build OK, ligne finale `Project build complete.` — confirme que le nouveau module et son test compilent et lient. (Si erreur `undefined reference` : vérifier le Step 4.)

- [ ] **Step 6 : (Matériel) Flasher le test_app et exécuter les tests `[lora_tx_packetize]`**

> Nécessite un device branché en USB. Si pas de device sous la main, passer ce step et le faire à la prochaine session hardware — le Step 5 a déjà garanti la compilation.

Run: `idf.py -C test_app -p /dev/cu.usbmodem101 flash monitor`
Au menu Unity, taper `[lora_tx_packetize]` puis Entrée.
Expected: `4 Tests 0 Failures 0 Ignored` pour les cas `packetize_small_tx_single_packet`, `packetize_large_tx_fragments`, `packetize_large_tx_roundtrip`, `packetize_null_args`.
Quitter le monitor : `Ctrl+]`.

- [ ] **Step 7 : Commit**

```bash
git add components/comm/lora_sync/include/comm/lora_tx_packetize.h \
        components/comm/lora_sync/src/lora_tx_packetize.c \
        components/comm/lora_sync/test/test_lora_tx_packetize.c \
        components/comm/lora_sync/CMakeLists.txt
git commit -m "$(cat <<'EOF'
feat(lora_sync): module lora_tx_packetize — emballage TX en paquets LoRa

Module pur (sans FreeRTOS/HAL) qui sérialise une transaction et produit
soit un paquet LORA_TX direct, soit N fragments LORA_FRAG via le
lora_frag_split() existant. 4 TEST_CASE Unity. Pas encore branché dans
lora_sync_do_cycle() — voir commit suivant.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 2 : Brancher `lora_tx_packetize` dans `lora_sync_do_cycle()`

**Files:**
- Modify: `components/comm/lora_sync/src/lora_sync.c`

- [ ] **Step 1 : Ajouter l'include du nouveau module**

Dans `components/comm/lora_sync/src/lora_sync.c`, après la ligne `#include "comm/lora_frag.h"` (ligne 20), ajouter :

```c
#include "comm/lora_tx_packetize.h"
```

- [ ] **Step 2 : Ajouter le compteur de séquence de fragmentation**

Toujours dans `lora_sync.c`, après la ligne `static lora_frag_ctx_t s_frag_ctx;` (ligne 39), ajouter :

```c

/*
 * Compteur de séquence pour la fragmentation des TX émises. Incrémenté à
 * chaque TX fragmentée pour que le réassembleur du récepteur distingue
 * deux séquences successives. Le wraparound 255->0 est sans conséquence :
 * deux séquences distinctes ne coexistent jamais assez longtemps pour
 * entrer en collision (timeout de réassemblage : 10 s).
 */
static uint8_t s_lora_tx_seq_id = 0;
```

- [ ] **Step 3 : Ajouter le helper d'émission d'une TX**

Toujours dans `lora_sync.c`, juste **avant** la définition de `void lora_sync_do_cycle(` (ligne 487), insérer :

```c
/*
 * Émet une transaction confirmée sur LoRa : un seul paquet LORA_TX si elle
 * tient dans COMM_MSG_LORA_MAX octets, sinon plusieurs fragments LORA_FRAG.
 *
 * Avant ce helper, lora_sync_do_cycle() empaquetait chaque TX dans un
 * buffer de 255 octets et ABANDONNAIT SILENCIEUSEMENT toute TX dont le
 * CBOR dépassait 254 octets — soit toute TX TRANSFER à 2 parents. La
 * fragmentation à l'émission (lora_frag_split) existait mais n'était
 * jamais appelée.
 */
static void lora_sync_send_one_tx(const lora_sync_config_t *config,
                                  const transaction_t *tx,
                                  uint32_t index, uint32_t total)
{
    uint8_t packets[LORA_FRAG_MAX_FRAGMENTS][LORA_FRAG_PACKET_MAX];
    size_t  packet_lens[LORA_FRAG_MAX_FRAGMENTS];
    uint8_t packet_count = 0;

    if (lora_tx_packetize(tx, s_lora_tx_seq_id,
                          packets, packet_lens, &packet_count) != 0) {
        ESP_LOGW(TAG, "TX %lu/%lu non émise : packetize a échoué "
                      "(sérialisation ou > %d fragments)",
                 (unsigned long)(index + 1), (unsigned long)total,
                 LORA_FRAG_MAX_FRAGMENTS);
        return;
    }

    /* Si la TX a été fragmentée, consommer un seq_id pour la suivante. */
    if (packet_count > 1) {
        s_lora_tx_seq_id++;
    }

    for (uint8_t p = 0; p < packet_count; p++) {
        hal_err_t err = config->lora->send(packets[p], packet_lens[p],
                                            config->lora->ctx);
        if (err != HAL_OK) {
            ESP_LOGW(TAG, "Échec envoi LoRa TX %lu/%lu (paquet %u/%u)",
                     (unsigned long)(index + 1), (unsigned long)total,
                     (unsigned)(p + 1), (unsigned)packet_count);
        }
    }
}

```

- [ ] **Step 4 : Remplacer la boucle d'envoi de `lora_sync_do_cycle()`**

Toujours dans `lora_sync.c`, dans `lora_sync_do_cycle()`, remplacer ce bloc (≈ lignes 573-585) :

```c
    uint8_t buf[COMM_MSG_LORA_MAX];
    for (uint32_t i = 0; i < tx_count; i++) {
        size_t buf_len;
        if (comm_msg_pack_lora_tx(buf, sizeof(buf),
                                   &tx_to_send[i], &buf_len) == 0) {
            hal_err_t err = config->lora->send(buf, buf_len,
                                                config->lora->ctx);
            if (err != HAL_OK) {
                ESP_LOGW(TAG, "Échec envoi LoRa TX %lu/%lu",
                         (unsigned long)(i + 1), (unsigned long)tx_count);
            }
        }
    }
```

par :

```c
    for (uint32_t i = 0; i < tx_count; i++) {
        lora_sync_send_one_tx(config, &tx_to_send[i], i, tx_count);
    }
```

> Note : `comm_msg_pack_lora_tx()` n'est plus appelé par `lora_sync.c` après ce changement. On le **laisse en place** dans `comm_protocol` : c'est une fonction publique de l'API wire, susceptible d'être utilisée ailleurs (tests, futur émetteur). Ne pas la supprimer.

- [ ] **Step 5 : Compiler le test_app**

Run: `idf.py -C test_app build`
Expected: build OK, `Project build complete.` — confirme que `lora_sync.c` compile toujours avec le nouvel include et le helper.

- [ ] **Step 6 : Compiler le firmware principal pour les deux cibles**

`lora_sync.c` est aussi compilé dans le firmware principal (cible ESP32 CYD ; sur ESP32-S3 le transport LoRa est un stub, mais le composant `lora_sync` reste lié). Vérifier les deux builds :

Run: `idf.py -B build-s3 build`
Expected: build OK (`Project build complete.`).

Run: `idf.py -B build-esp32 -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.esp32" build`
Expected: build OK (`Project build complete.`).

> Ces deux commandes sont celles documentées dans la note Doctech `12 - Refactoring main.c (Lot D)`. Si la config sdkconfig a évolué depuis, adapter en conséquence.

- [ ] **Step 7 : (Matériel) Re-flasher le test_app et relancer les tests LoRa**

> Nécessite un device. Sinon, à reporter à la prochaine session hardware.

Run: `idf.py -C test_app -p /dev/cu.usbmodem101 flash monitor`
Au menu Unity, taper `[lora_frag]` puis `[lora_tx_packetize]`.
Expected: tous PASS — la fragmentation réception (`[lora_frag]`) n'est pas régressée et l'émission (`[lora_tx_packetize]`) fonctionne.
Quitter : `Ctrl+]`.

- [ ] **Step 8 : Commit**

```bash
git add components/comm/lora_sync/src/lora_sync.c
git commit -m "$(cat <<'EOF'
fix(lora_sync): brancher la fragmentation à l'émission des TX confirmées

lora_sync_do_cycle() empaquetait chaque TX dans un buffer de 255 octets
et abandonnait silencieusement toute TX dont le CBOR dépassait 254 octets
(toute TRANSFER à 2 parents — TX_CBOR_MAX_SIZE vaut 320). lora_frag_split()
existait mais n'était jamais appelé.

Désormais lora_sync_send_one_tx() appelle lora_tx_packetize() : paquet
LORA_TX direct ou fragments LORA_FRAG. Un échec d'emballage est loggé en
ESP_LOGW au lieu d'être silencieux.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 3 : Doctech — corriger la note `04 - Décisions techniques`

**Files:**
- Modify (vault Obsidian) : `/Users/misterniark/Documents/Obsidian/Misterniark/Projet/Mesh Pay/Doctech/04 - Décisions techniques.md`

> Pas de commit git : fichier hors du dépôt firmware.

- [ ] **Step 1 : Corriger la mention de contrainte ESP-NOW dans la liste CBOR**

Edit — remplacer :

```
- Format compact et **binaire** (important pour la contrainte 250 octets ESP-NOW)
```

par :

```
- Format compact et **binaire** (essentiel face au plafond de taille de paquet : 320 octets en ESP-NOW V2, 255 en LoRa)
```

- [ ] **Step 2 : Corriger la taille de TX sérialisée**

Edit — remplacer :

```
Taille d'une TX sérialisée : ~233 octets (sous 250 ✅).
```

par :

```
Taille de la struct `transaction_t` en mémoire : ~233 octets. Sérialisée en CBOR, une TX complète peut atteindre **`TX_CBOR_MAX_SIZE` = 320 octets** (cas d'une TRANSFER à 2 parents, ~282 octets) — le projet est passé à **ESP-NOW V2** (plafond 320 o, contre 250 o en V1, Lot E.1bis) pour l'accommoder. Côté **LoRa**, le paquet reste plafonné à 255 octets : une TX dont le CBOR dépasse ~254 octets est donc **fragmentée** en messages `LORA_FRAG` à l'émission (fix 2026-05-14 — voir [[08 - Journal des corrections récentes]]).
```

- [ ] **Step 3 : Corriger la ligne « Fragmentation LoRa » de la table des décisions**

Edit — remplacer :

```
| Fragmentation LoRa | Hybride (1 TX / paquet + mode rattrapage) |
```

par :

```
| Fragmentation LoRa | Hybride : 1 paquet `LORA_TX` si CBOR ≤ 255 o, sinon découpe en `LORA_FRAG`. Émission **et** réception câblées depuis le fix 2026-05-14 |
```

---

## Task 4 : Doctech — ajouter l'entrée de dette « Gossip LoRa fragile » dans la note `07`

**Files:**
- Modify (vault Obsidian) : `/Users/misterniark/Documents/Obsidian/Misterniark/Projet/Mesh Pay/Doctech/07 - Dette technique.md`

> Pas de commit git : fichier hors du dépôt firmware.

- [ ] **Step 1 : Insérer la nouvelle section de dette**

Edit — remplacer :

```
## 🟢 ESP32-S3 sans LoRa

**Statut** : choix actuel, évolutif
```

par :

```
## 🟡 Propagation DAG LoRa fragile — filigrane unique, ni relais ni anti-entropie

**Statut** : limité mais fonctionnel pour un petit réseau ; à durcir avant un test terrain à plusieurs sauts.

**Problème** : la convergence du DAG via LoRa repose entièrement sur la re-diffusion périodique (`lora_sync_do_cycle`, toutes les 2 min). Trois faiblesses identifiées en lisant `components/comm/lora_sync/src/lora_sync.c` et `main/transport/transport_lora.c` :

1. **Aucun relais explicite.** Un `LORA_TX` reçu est validé puis posté à `core_task` — il n'est jamais ré-émis. Pas de cache `seen` ni de TTL pour les TX (contrairement aux broadcasts et PING). La propagation multi-saut n'existe que comme effet de bord de la re-diffusion périodique.
2. **Filigrane temporel unique.** `lora_collect_confirmed_txs` ne renvoie que les TX `CONFIRMED` dont `timestamp > last_sync_ts`, et `last_sync_ts` ne fait qu'avancer. Une TX qui arrive « en retard » (timestamp ≤ filigrane courant) n'est **jamais re-diffusée** par ce device — fréquent en mode Lamport où le timestamp est un compteur logique. La TX meurt sur ce nœud.
3. **Troncature silencieuse à 32 TX/cycle.** `SYNC_MAX_TX_PER_CYCLE = 32` : si plus de 32 TX dépassent le filigrane, seules 32 partent et le filigrane avance quand même → les TX non envoyées peuvent être définitivement exclues de la sync.

**Conséquence** : sur un réseau de 2-3 devices en visibilité directe, ça converge. Au-delà (multi-saut, paquets perdus, trafic dense), la convergence n'est pas garantie. Aucun mécanisme d'anti-entropie (« envoie-moi ce qui me manque »).

**Pistes** (chantier de conception dédié, pas un simple fix — à brainstormer) :
- Relais des `LORA_TX` avec cache de signatures vues + délai aléatoire, sur le modèle des broadcasts.
- Remplacer le filigrane unique par un suivi par-TX (set de `tx_id` déjà diffusés) ou un vrai protocole d'anti-entropie (échange de résumés de DAG).
- Corriger la troncature : n'avancer le filigrane que jusqu'au plus ancien non envoyé.

**Pourquoi pas fait maintenant** : la fragmentation à l'émission (prérequis pour que les grosses TX circulent tout court) a été traitée d'abord (fix 2026-05-14) ; le durcissement du gossip est un chantier à part entière.

Voir [[08 - Journal des corrections récentes]] (fix fragmentation LoRa).

---

## 🟢 ESP32-S3 sans LoRa

**Statut** : choix actuel, évolutif
```

- [ ] **Step 2 : Ajouter la ligne au tableau « Résumé par priorité »**

Edit — remplacer :

```
| 🟢 | [[#🟢 ESP32-S3 sans LoRa\|ESP32-S3 sans LoRa]] | Sync longue portée absente |
```

par :

```
| 🟡 | [[#🟡 Propagation DAG LoRa fragile filigrane unique ni relais ni anti-entropie\|Gossip LoRa fragile]] | Convergence multi-saut non garantie |
| 🟢 | [[#🟢 ESP32-S3 sans LoRa\|ESP32-S3 sans LoRa]] | Sync longue portée absente |
```

---

## Task 5 : Doctech — entrée de journal du fix dans la note `08`

**Files:**
- Modify (vault Obsidian) : `/Users/misterniark/Documents/Obsidian/Misterniark/Projet/Mesh Pay/Doctech/08 - Journal des corrections récentes.md`

> Pas de commit git : fichier hors du dépôt firmware.

- [ ] **Step 1 : Mettre à jour la date du frontmatter**

Edit — remplacer :

```
  - Audit
Date: 2026-05-11
---
```

par :

```
  - Audit
Date: 2026-05-14
---
```

- [ ] **Step 2 : Insérer la section de journal avant « Voir aussi »**

Edit — remplacer :

```
## Voir aussi

- [[12 - Refactoring main.c (Lot D)]] — plan complet et avancement
```

par :

```
# Fix propagation LoRa — fragmentation à l'émission (2026-05-14)

> [!warning] Bug : grosses TX silencieusement non propagées en LoRa
> En relisant `components/comm/lora_sync/`, deux constats : (1) `lora_frag_split()` (fragmentation à l'émission) était implémenté et testé mais **jamais appelé** en production ; (2) `lora_sync_do_cycle` empaquetait chaque TX dans un buffer de 255 octets via `comm_msg_pack_lora_tx` et, en cas d'échec (CBOR > 254 octets), **abandonnait la TX sans aucun log**. Or `TX_CBOR_MAX_SIZE = 320` : toute TX TRANSFER à 2 parents (~282 octets de CBOR) était indiffusable en LoRa.

## Cause racine

Seul le **réassemblage** (`LORA_FRAG` en réception) était câblé. Le chemin d'émission n'avait jamais été relié à `lora_frag_split`. Le plafond CBOR ayant été monté de 250 à 320 (ESP-NOW V2, Lot E.1bis), des TX parfaitement valides et transmissibles en ESP-NOW devenaient invisibles au reste du réseau via LoRa.

## Correction

- Nouveau module pur `components/comm/lora_sync/src/lora_tx_packetize.{c,h}` : `lora_tx_packetize()` sérialise la TX et renvoie soit 1 paquet direct `LORA_TX` (si ≤ 255 o), soit N fragments `LORA_FRAG` via `lora_frag_split()`. Aucune dépendance FreeRTOS/HAL → testable en natif.
- `lora_sync_do_cycle` appelle un helper `lora_sync_send_one_tx()` qui émet le(s) paquet(s) ; un échec d'emballage est désormais **loggé** (`ESP_LOGW`) au lieu d'être silencieux.
- Compteur de séquence `s_lora_tx_seq_id` pour distinguer les séquences de fragments.
- 4 TEST_CASE ajoutés (`test/test_lora_tx_packetize.c`) : paquet unique, fragmentation, round-trip émission→réassemblage→`tx_deserialize`, args NULL.

## Asymétrie de format (important)

Le paquet direct est `[COMM_MSG_LORA_TX][cbor]` (le récepteur appelle `comm_msg_unpack_lora_tx` sur le tout). Les fragments contiennent le **CBOR nu** sans byte de type (le récepteur réassemble puis appelle `tx_deserialize` directement). `lora_tx_packetize` respecte cette asymétrie.

## Limite non traitée

Ce fix permet aux grosses TX de **circuler** ; il ne corrige pas la fragilité du gossip lui-même (filigrane unique, pas de relais, troncature à 32 TX/cycle) — voir [[07 - Dette technique#🟡 Propagation DAG LoRa fragile filigrane unique ni relais ni anti-entropie|la dette dédiée]].

**Fichiers** : `components/comm/lora_sync/src/lora_tx_packetize.c` (nouveau), `components/comm/lora_sync/include/comm/lora_tx_packetize.h` (nouveau), `components/comm/lora_sync/src/lora_sync.c`, `components/comm/lora_sync/CMakeLists.txt`, `components/comm/lora_sync/test/test_lora_tx_packetize.c` (nouveau)

---

## Voir aussi

- [[12 - Refactoring main.c (Lot D)]] — plan complet et avancement
```

---

## Self-Review

**1. Couverture du périmètre.**
- Point 1 (bug fragmentation émission) → Tasks 1-2. ✅
- Point 2 (doc note 04 + note 07) → Task 3 (note 04) + Task 4 (note 07). ✅
- Point 3 (fragilité gossip → entrée de dette) → Task 4, section 🟡 dédiée. ✅
- Convention projet (chaque fix journalisé) → Task 5. ✅

**2. Placeholders.** Aucun « TBD », aucun « gérer les cas limites » abstrait : chaque step de code donne le contenu exact ; chaque step de doc donne l'`old_string`/`new_string` exact.

**3. Cohérence des types/signatures.** `lora_tx_packetize(const transaction_t *, uint8_t, uint8_t [][LORA_FRAG_PACKET_MAX], size_t *, uint8_t *)` — signature identique dans le header (Task 1 Step 2), l'implémentation (Step 3), les tests (Step 1) et l'appelant `lora_sync_send_one_tx` (Task 2 Step 3). `s_lora_tx_seq_id` (uint8_t) déclaré Task 2 Step 2, utilisé Step 3. `lora_frag_split`, `lora_frag_receive`, `lora_frag_get_result`, `lora_frag_ctx_init`, `LORA_FRAG_HEADER_SIZE`, `LORA_FRAG_PAYLOAD_MAX`, `LORA_FRAG_PACKET_MAX`, `LORA_FRAG_MAX_FRAGMENTS` : signatures et constantes existantes du composant (`lora_frag.h`), réutilisées telles quelles.

**Risques connus / points de vigilance pour l'exécutant :**
- *Test `packetize_large_tx_*`* : repose sur le fait qu'une TRANSFER à 2 parents produit un CBOR > 254 octets. Estimation ~282 o — confortablement au-dessus. Si `TEST_ASSERT_GREATER_OR_EQUAL(2, count)` échoue, c'est que `tx_serialize_full` est plus compact que prévu : le bug d'origine serait alors inatteignable (info utile, pas un échec du plan).
- *`tx_serialize_full` sur une struct construite à la main* (`build_large_tx`/`build_small_tx`) : la fonction sérialise les champs sans valider signature ni solde ; `type`/`parent_count`/`status` sont mis à des valeurs valides. Si malgré tout elle rejette la struct, ajuster en construisant la TX via `tx_create_transfer` (nécessiterait alors `crypto` dans les `REQUIRES` du test et un keypair généré).
- *Pile de `lora_sync_send_one_tx`* : le tableau `packets[16][255]` (~4 Ko) vit sur la pile de `lora_task` (6144 mots = 24 Ko), en plus de `tx_to_send[32]` (~7,5 Ko) dans `lora_sync_do_cycle`. Pic ~12 Ko : dans la marge, mais à surveiller via la tâche `stkmon` après flash.
- *Numéros de ligne de `lora_sync.c`* (487, 573-585, etc.) : valables à l'état actuel du fichier. Si le fichier a bougé, se repérer sur le **contenu exact** des `old_string` plutôt que sur les numéros.

---

## Execution Handoff

**Plan complet et sauvegardé dans `docs/superpowers/plans/2026-05-14-lora-dag-propagation.md`. Deux options d'exécution :**

**1. Subagent-Driven (recommandé)** — un sous-agent neuf par tâche, revue entre chaque, itération rapide.

**2. Inline Execution** — exécution des tâches dans cette session, par lots avec points de contrôle.

**Quelle approche ?** (À me dire « tout à l'heure » quand tu reviens — d'ici là le plan est prêt et n'attend que le feu vert.)
