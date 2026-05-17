# Specs — Système de paiement décentralisé offline

> Document vivant — mis à jour au fil des décisions validées.

## 1. Vue d'ensemble

Système de paiement décentralisé fonctionnant **sans connexion internet**, basé sur des devices ESP32 équipés d'un écran tactile. Les paiements s'effectuent de proche en proche, et la synchronisation globale du réseau repose sur la radio longue portée.

**Cas d'usage** : commerce local (marchés, villages, festivals) et transferts P2P entre individus.

## 2. Décisions validées

| Décision | Choix | Raison |
|---|---|---|
| Ledger | **DAG** (Directed Acyclic Graph) | Parallélisme natif, léger pour ESP32, adapté au offline, sync LoRa par fusion de sous-graphes |
| Double-spend | **Solde verrouillé** | Le solde est verrouillé après émission jusqu'à confirmation par au moins 1 peer — empêche le double-spend à la source |
| Communication paiement | **ESP-NOW** | Latence ~ms, intégré ESP32, pas d'appairage, portée ~200m |
| Synchronisation réseau | **LoRa** | Longue portée (km), basse consommation, adapté aux deltas légers |
| Cible | **Polyvalent** | Commerce local + P2P personnel |
| Sérialisation | **CBOR** | Standard, compact, libs C légères (tinycbor), auto-descriptif, extensible, éprouvé en IoT |
| Approvisionnement | **Device maître** | Un device admin peut créer des crédits et les distribuer. Simple, contrôlable |
| Timeout verrouillage | **30 secondes** | Équilibre entre réactivité et tolérance aux délais ESP-NOW |
| Élagage DAG | **Hybride** | Checkpoints en Flash + fenêtre glissante des TX récentes en RAM |
| Sync LoRa | **Toutes les 2 min** | Équilibre consommation/réactivité, sans surcharger le canal |
| Fenêtre RAM | **250 TX** (~62 KB) | Réduit de 500 à 250 pour tenir dans la DRAM ESP32 avec l'UI et les comms |
| Checkpoints | **À 80% (200 TX)** | Déclenché avant saturation de la fenêtre (250 TX max) pour éviter de bloquer les insertions pendant la consolidation |
| Unité monétaire | **Abstraite** ("crédits") | Unité propre au réseau, valeur définie par les utilisateurs, pas de référence à une devise réelle |
| Découverte peers | **Broadcast ESP-NOW** | Le payeur broadcast, les devices à portée répondent avec leur clé publique. Sans configuration |
| Fragmentation LoRa | **Hybride** | 1 TX par paquet par défaut. Fragmentation uniquement pour checkpoints et syncs de rattrapage |
| Stockage clés | **NVS chiffré** | Partition NVS chiffrée native ESP-IDF (flash encryption). Bonne sécurité sans hardware supplémentaire |
| Fusion TX réseau | **dag_merge_transaction()** | Encapsule doublon/structure/signature/MINT en un seul appel, remplace la validation inline |
| Seuil checkpoint | **dag_needs_checkpoint()** | Basé sur la capacité réelle du DAG (80%) au lieu d'un compteur d'insertions — plus robuste |
| Élagage post-checkpoint | **dag_prune_before()** | Supprime les TX consolidées après chaque checkpoint, empêche le DAG de saturer |
| Persistance checkpoint | **Callbacks injectables** | checkpoint_save_fn / checkpoint_load_fn — découple la logique métier du backend de stockage (NVS, SPIFFS, mémoire pour tests) |
| Solde pubkey arbitraire | **wallet_get_balance_for() [C2]** | Utilise checkpoint+DAG post-checkpoint pour calculer le solde de n'importe quelle pubkey. Défense en profondeur, pas une garantie forte (limite décrite dans le commentaire du header) |
| Anti double-dépense | **Nonce monotone par émetteur [I3]** | Champ `seq` (uint32) dans transaction_t, incrémenté à chaque TX émise. Deux TX avec même `(from, seq)` mais id différent → `DAG_MERGE_CONFLICT`. Persistance NVS du compteur pour survivre aux reboots |
| Finalité LoRa | **Attestation signée [I2]** | Nouveau message `COMM_MSG_LORA_ATTESTATION` (0x18) diffusé par le destinataire après confirmation. Signature Ed25519 de `tx_id`. Permet au réseau hors portée ESP-NOW de promouvoir une TX de LOCKED à CONFIRMED de façon prouvée |
| Signature PONG | **Obligatoire [I4]** | Les PONG LoRa sont signés par le device qui répond. Empêche l'usurpation d'identité dans les listes de scan/rename/forward |
| Rate-limit ESP-NOW | **Par-MAC + global [I5]** | Table LRU de 8 MACs × compteur par MAC. Un pair bruyant ne bloque plus les messages des autres. Plafond global conservé comme filet anti-flood massif |
| Persistance secrets | **Hors arbre source [I6/I7]** | `secure_boot_signing_key.pem` jamais versionné (.gitignore), procédure de production documentée. `.env` ignoré pour éviter les fuites de credentials |
| Architecture logicielle | **Par couche** | hal/ (drivers), core/ (DAG, crypto, wallet), comm/ (ESP-NOW, LoRa), ui/ (interface). Clair et testable |
| UI | **9 écrans** | Accueil, Payer, Historique, Paramètres, Admin, MINT, Broadcast, Renommer, Forward |
| Mises à jour | **OTA ponctuel** | Via Wi-Fi ou USB temporaire. Partition OTA ESP-IDF avec rollback |
| Multi-maître | **Plusieurs indépendants** | Plusieurs devices admin peuvent MINT chacun. Redondance, pas de point de défaillance unique |
| Librairie UI | **LVGL** | Standard embarqué, widgets riches, support tactile natif, bonne intégration ESP-IDF |
| Tâches FreeRTOS | **4 tâches** | ui_task, espnow_task, lora_task, core_task. Séparation claire des responsabilités |
| Signature OTA | **Clé du device maître** | Réutilise la clé maître pour signer les firmwares. Moins de clés à gérer |
| Broadcast texte maître | **Signé Ed25519, 157 chars max** | Le maître envoie un message texte LoRa signé (authentification). 157 chars max car la signature Ed25519 (64 octets) + pubkey (32) occupent de l'espace dans le paquet LoRa de 255 octets |
| Relay broadcast | **Single-hop, délai aléatoire** | Chaque device relaye les broadcasts validés via LoRa pour étendre la portée. Cache de 16 signatures pour éviter les boucles. Délai 200-1000ms anti-collision |
| Ping/Pong LoRa | **PING relayé, PONG direct** | Le maître envoie un PING, les devices répondent par PONG. Seul le PING est relayé (single-hop) pour limiter le trafic. PONGs envoyés avec délai aléatoire 1-5s |
| Code PIN | **4 chiffres, PBKDF2-HMAC-SHA256 + sel** | Protège les actions sensibles (paiement, MINT, modif alias). 10 000 itérations PBKDF2 avec sel aléatoire 16 octets. Délai progressif anti brute-force, blocage après 10 échecs. Effacement NVS = device verrouillé |
| Renommage distant | **LORA_SET_ALIAS signé par le maître** | Le maître peut renommer un device client via LoRa. Nécessaire car le Waveshare 1.47" n'a pas de clavier. Message signé Ed25519 pour empêcher l'usurpation |
| Saisie PIN Waveshare | **Style Ledger (◄ ► horizontal)** | Écran trop petit pour un pavé numérique. ◄ = décrémenter, ► = incrémenter, bouton Valider en dessous. Layout horizontal pour le paysage 320×172 |
| Auto-forward bénéficiaire | **Batch forward périodique (configurable)** | Mode encaissement festival : le device transfère son solde vers un bénéficiaire toutes les N minutes. TX auto-confirmée, propagée via LoRa sync. Configurable par le maître via LORA_SET_BENEFICIARY (0x17) |
| Frais de transfert | **Redirigés vers le mint_authority** | Les frais sont crédités au premier `mint_authority` (commission pour l'organisateur du réseau). Si aucun `fee_recipient` n'est configuré, les frais sont brûlés (comportement historique). La fonte (demurrage) reste détruite — ce sont deux mécanismes distincts |

## 3. Architecture matérielle

### Plateforme de prototypage
- **Board** : ESP32-2432S028 "Cheap Yellow Display" (CYD)
- **MCU** : ESP32 classique (dual-core Xtensa LX6, 520 KB RAM, 4 MB Flash)
- **Écran** : 2.8" TFT ILI9341 (320x240, SPI)
- **Tactile** : résistif XPT2046 (SPI)
- **LED RGB** : intégrée (feedback visuel paiement)
- **Slot SD** : disponible (stockage supplémentaire possible)
- **Module LoRa** : Grove Wio-E5 (chip STM32WLE5JC, interface **UART**, commandes **AT**)
- **Alimentation** : USB (prototype)
- **ESP-IDF** : v5.4

## 4. Architecture logicielle

### 4.1 DAG — Structure de données

Chaque transaction est un noeud du DAG qui référence une ou plusieurs transactions parentes.

```
[TX-A] ──┐
          ├──> [TX-C] ──> [TX-D]
[TX-B] ──┘
```

**Structure d'une transaction** (à affiner) :
- `id` : hash SHA-256 de la transaction
- `type` : `TRANSFER` | `MINT` (seul le device maître peut émettre `MINT`)
- `from` : clé publique de l'émetteur (clé du maître signataire pour `MINT`)
- `to` : clé publique du destinataire
- `amount` : montant en **crédits** (uint32, unité abstraite)
- `parents[]` : références aux transactions parentes (hashes)
- `timestamp` : horodatage signé
- `signature` : signature Ed25519 de l'émetteur
- `status` : `LOCKED` | `CONFIRMED` | `CANCELLED`

### 4.2 Découverte des peers — Broadcast ESP-NOW

```
Device A (payeur)                    Devices à portée
      │                                      │
      ├─── 1. Broadcast DISCOVER ───────────>│ (tous les devices à portée)
      │                                      │
      │<── 2. Réponse ANNOUNCE ─────────────┤ Device B (clé publique + alias)
      │<── 2. Réponse ANNOUNCE ─────────────┤ Device C (clé publique + alias)
      │                                      │
      ├─── 3. Sélection du destinataire      │
      │       sur l'écran tactile            │
```

- **DISCOVER** : broadcast sur l'adresse FF:FF:FF:FF:FF:FF, contient la clé publique de A
- **ANNOUNCE** : réponse unicast signée Ed25519, format `[0x02][pubkey:32][sig:64][nonce:4][alias_len:1][alias:N]` — le nonce aléatoire est vérifié via un cache anti-rejeu de 32 entrées
- **Timeout** : 5 secondes pour collecter les réponses

### 4.3 Flux de paiement — Modèle "Solde verrouillé"

```
Device A (payeur)                    Device B (receveur)
      │                                      │
      ├─── 1. Crée TX (status=LOCKED) ──────>│
      │         via ESP-NOW                  │
      │                                      ├─── 2. Valide la TX
      │                                      │       (vérifie solde, signature)
      │                                      │
      │<── 3. Envoie ACK ───────────────────┤
      │         via ESP-NOW                  │
      ├─── 4. Finalise TX (status=CONFIRMED) │
      │                                      ├─── 4. Crédite le solde
      │                                      │
```

- **Verrouillage** : le montant est déduit immédiatement du solde disponible de A
- **Timeout** : si pas d'ACK après **30 secondes**, A peut annuler (`CANCELLED`) et déverrouiller
- **Garantie** : A ne peut pas émettre une autre TX avec les mêmes fonds tant qu'ils sont verrouillés

### 4.4 Synchronisation LoRa

- Chaque device diffuse ses transactions `CONFIRMED` récentes **toutes les ~2 minutes ± 25 % (jitter, cf. anti-collision)**
- Les devices récepteurs **valident** via `dag_merge_transaction()` (structure + signature + autorité MINT) puis fusionnent le sous-graphe reçu dans leur DAG local
- La validation currency (`currency_validate()`) est effectuée **avant** le merge pour vérifier les règles métier (plafond, expiration, montants)
- Résolution de conflits : si deux TX conflictuelles apparaissent, la TX `CONFIRMED` la plus ancienne prévaut

**Anti-collision — jitter aléatoire (boot + cycle) :**
- **Problème observé** : sans jitter, deux devices bootés en quasi-simultané (USB hub, mise sous tension groupée) entraient en cycle de sync au même tick → émission LoRa simultanée → ni l'un ni l'autre ne reçoit l'autre (SX1262 sourd pendant son propre TX) → le DAG ne se propage jamais entre eux.
- **Jitter de boot** : avant le tout premier cycle, chaque device dort un délai aléatoire uniformément distribué dans `[0, sync_interval_ms]`. Décorrèle l'instant du premier broadcast entre devices co-bootés.
- **Jitter par cycle** : à chaque itération, le délai est tiré dans `[interval − 25 %, interval + 25 %]`. Maintient la décorrélation au fil des cycles ; deux devices qui auraient fini par re-converger se redécalent.
- Calcul implémenté dans les helpers PURS `lora_jitter_initial_ms()` / `lora_jitter_around_ms()` (header privé `components/comm/lora_sync/src/lora_sync_jitter.h`), couverts par 11 tests unitaires (`test/test_lora_sync_jitter.c`).
- L'aléa est fourni par `esp_random()` à l'appel ; les helpers eux-mêmes n'invoquent aucune API ESP-IDF (testables sans mock).

**Stratégie de fragmentation hybride :**
- **Mode normal** : 1 TX confirmée par paquet LoRa (toujours < 255 octets)
- **Mode rattrapage** : quand un device rejoint le réseau ou a un retard important, fragmentation avec header `[fragment_index | total_fragments | payload]`
- **Checkpoints** : les snapshots de soldes peuvent être fragmentés pour la sync initiale

### 4.5 Sérialisation CBOR

- Librairie : **tinycbor** (légère, compatible ESP-IDF)
- Chaque transaction sérialisée doit tenir dans **250 octets** (contrainte ESP-NOW)
- Format auto-descriptif et extensible — permet d'ajouter des champs sans casser la compatibilité

### 4.6 Élagage du DAG — Modèle hybride

```
┌──────────────────────────────────────────────┐
│                   Flash                       │
│  [Checkpoint N-2] [Checkpoint N-1] [Chk N]   │
│   (snapshot des     (snapshot des   (dernier  │
│    soldes)           soldes)        snapshot)  │
└──────────────────────────────────────────────┘
┌──────────────────────────────────────────────┐
│                    RAM (~125 KB)              │
│  Fenêtre glissante : 250 TX max              │
│  [TX-1] → [TX-2] → [TX-3] → ... → [TX-250] │
│  Tips du DAG (feuilles non référencées)       │
└──────────────────────────────────────────────┘
```

- **Checkpoints** : déclenchés par `dag_needs_checkpoint()` à **80% de capacité (200 TX)** — les soldes confirmés sont consolidés en un snapshot sauvé en Flash, avant saturation de la fenêtre
- **Fenêtre glissante** : max **250 TX** en RAM (~62 KB), les transactions depuis le dernier checkpoint
- **Purge** : au checkpoint, `dag_prune_before()` supprime les TX consolidées (timestamp ≤ checkpoint) de la RAM
- **Cycle** : fenêtre 80% → `checkpoint_create()` + fonte → `dag_prune_before()` → fenêtre allégée
- **Persistance injectable** : la sauvegarde/chargement des checkpoints utilise les callbacks `checkpoint_save_fn` / `checkpoint_load_fn` (NVS par défaut, substituable pour tests ou autre backend)

### 4.7 Approvisionnement — Device maître

- Un device désigné **admin** possède une clé maître capable de créer des crédits (transaction de type `MINT`)
- Les transactions `MINT` sont signées par la clé maître (`from` = clé publique du maître signataire)
- Les autres devices ne peuvent que transférer des crédits existants
- **Plusieurs devices maîtres** indépendants : chacun peut MINT de son côté
- Chaque device maître est identifié par sa clé publique, inscrite dans une liste de clés autorisées
- Les devices normaux valident un MINT en vérifiant que la signature provient d'une clé maître connue

### 4.8 Cryptographie & stockage des clés

- **Identité** : paire de clés du profil Mesh Pay par device (`meshpay-monocypher-4.0.2-ed25519-closed`, non annoncé RFC8032)
- **Initialisation** : `crypto_init()` garde l'invariant d'init unique ; Monocypher 4.0.2 n'a pas d'état global
- **Signature** : chaque transaction est signée par l'émetteur (TRANSFER et MINT)
- **Vérification MINT** : la signature est vérifiée avec `tx->from`, puis la clé est validée contre `mint_authorities`
- **Hash** : SHA-256 pour les identifiants de transactions
- **Comparaisons** : `public_key_equal()` et `hash_equal()` sont à temps constant (XOR accumulator, anti timing-attack)
- **Zéroisation** : tous les buffers de clé privée sur la pile sont effacés via `mbedtls_platform_zeroize()` après usage (y compris sur les chemins d'erreur)
- **Import de keypair** : vérifié cryptographiquement par test sign/verify (pas juste une comparaison mémoire)
- **Stockage** : clés privées dans la **partition NVS chiffrée** (flash encryption ESP-IDF)
- **Flash Encryption** : AES-256, activé en mode développement (`CONFIG_SECURE_FLASH_ENC_ENABLED`). Pour la production : passer en `ENCRYPTION_MODE_RELEASE` (brûle les eFuses, interdit reflashing en clair)
- **NVS Encryption** : AES-XTS via `nvs_sec_provider` (schéma flash-encryption). Les clés sont stockées dans la partition `nvs_keys` (4KB, flag `encrypted`), générées automatiquement au premier boot
- **Partition table** : offset 0x10000 pour laisser 60KB au bootloader avec flash encryption. Layout : NVS 0x11000, otadata 0x17000, phy_init 0x19000, OTA 0x20000
- **Secure Boot V2** : documenté mais non activé par défaut (nécessite RSA-3072 + eFuses irréversibles). ESP32 requiert ECO3+, ESP32-S3 supporte nativement
- **Gestion de la clé de signature (I6)** : `secure_boot_signing_key.pem` ne doit JAMAIS être versionnée. Le `.gitignore` exclut `*.pem`. Procédure de production :
  1. Générer la clé **hors de l'arbre source** : `espsecure.py generate_signing_key --version 2 --scheme rsa3072 /chemin/securise/secure_boot_signing_key.pem`
  2. Stocker la clé dans un gestionnaire de secrets (vault, HSM, etc.) ou sur un support chiffré
  3. Référencer son chemin absolu via `CONFIG_SECURE_BOOT_SIGNING_KEY` sans la copier dans le repo
  4. Si la clé présente dans le repo a déjà été publiée (git push), la considérer compromise et en régénérer une autre — l'ancienne doit être révoquée via eFuse (irréversible)
- La clé de chiffrement NVS est dérivée de l'eFuse du chip (unique par device, non extractible)
- **Isolation** : la clé privée n'est jamais exposée à la couche UI (seule la clé publique est passée via `own_pubkey`)

### 4.9 Interface utilisateur — 9 écrans

```
┌─────────────┐     ┌─────────────┐     ┌─────────────┐     ┌─────────────┐
│   ACCUEIL   │────>│   PAYER     │     │ HISTORIQUE  │     │ PARAMETRES  │
│             │     │             │     │             │     │             │
│  Solde: 500 │     │ Montant: _  │     │ TX-1  +50   │     │ [Receive]   │
│  Alias: Bob │     │ Dest: Alice │     │ TX-2  -20   │     │ [Admin]     │
│             │     │ [Confirmer] │     │ TX-3  -10   │     │ [PIN]       │
│ [Payer]     │     │ [Annuler]   │     │ ...         │     └─────────────┘
│ [Historique]│     └─────────────┘     └─────────────┘            │
│ [Paramètres]│                                              (si is_master)
└─────────────┘                                                    ▼
                    ┌─────────────┐     ┌─────────────┐     ┌─────────────┐
                    │   ADMIN     │────>│    MINT     │     │  BROADCAST  │
                    │  (maître)   │     │             │     │             │
                    │ [MINT]      │     │ Montant: _  │     │ Texte reçu  │
                    │ [Broadcast] │     │ Dest: ___   │     │ du maître   │
                    │ [Scan]      │     │ [Créer]     │     │             │
                    │ [Renommer]  │     └─────────────┘     └─────────────┘
                    │ [Forward]   │
                    └─────────────┘     ┌─────────────┐     ┌─────────────┐
                                        │  RENOMMER   │     │  FORWARD    │
                                        │             │     │             │
                                        │ Device: ___ │     │ Device: ___ │
                                        │ Alias: ___  │     │ Benef: ___  │
                                        │ [Renommer]  │     │ Intv: 5min  │
                                        └─────────────┘     │ [Configurer]│
                                                            └─────────────┘
```

- **Accueil** : solde disponible, alias du device, accès aux autres écrans
- **Payer** : saisie du montant → broadcast discover → sélection du destinataire → confirmation → feedback toast (succès/échec/fonds insuffisants)
- **Historique** : liste scrollable des TX récentes (fenêtre glissante), avec montant, pair, statut et timestamp
- **Paramètres** : informations device (clé publique, nombre TX, version), accès receive/admin/PIN
- **Admin** : accessible uniquement si `is_master == true` (vérifié par l'UI). Menu vers MINT, broadcast, scan, renommer, forward
- **MINT** : roller destinataire + saisie montant → création de crédits
- **Broadcast** : affichage des messages texte reçus du maître (auto-navigation quand un broadcast arrive)
- **Renommer** : sélection device + saisie alias → envoi `LORA_SET_ALIAS` (grand écran uniquement)
- **Forward** : 3 rollers (device, bénéficiaire, intervalle) → envoi `LORA_SET_BENEFICIARY` (tous écrans)
- **Librairie** : **LVGL 9.x** — rendu optimisé pour MCU, widgets natifs (boutons, rollers, clavier), support tactile intégré

### 4.10 Architecture des modules (composants ESP-IDF)

```
project/
├── components/
│   ├── hal/                  # Couche abstraction matérielle
│   │   ├── display/          # Driver écran tactile
│   │   ├── lora/             # Driver module LoRa (SPI)
│   │   └── storage/          # Abstraction NVS / Flash
│   │
│   ├── core/                 # Logique métier
│   │   ├── dag/              # Structure DAG, validation, élagage
│   │   ├── wallet/           # Gestion solde, verrouillage, checkpoints
│   │   ├── crypto/           # Profil signature Mesh Pay, SHA-256, gestion des clés
│   │   └── transaction/      # Création, sérialisation CBOR, statuts
│   │
│   ├── comm/                 # Communications
│   │   ├── espnow/           # Discover, paiement, ACK
│   │   └── lora_sync/        # Sync périodique, fragmentation, fusion
│   │
│   └── ui/                   # Interface utilisateur
│       ├── screens/          # Accueil, Payer, Historique, Admin
│       ├── widgets/          # Boutons, listes, clavier numérique
│       └── navigation/       # Gestion des écrans, transitions
│
├── main/                     # Point d'entrée, init, tâches FreeRTOS
├── partitions.csv            # Table de partitions (NVS, OTA, app)
└── CMakeLists.txt
```

- Chaque composant a son propre `CMakeLists.txt` et ses dépendances déclarées
- Les tests unitaires sont dans un dossier `test/` par composant
- La couche `core/` ne dépend **jamais** de `hal/` directement — injection via interfaces

### 4.11 Mises à jour OTA

- **Mécanisme** : partition OTA standard ESP-IDF (scheme `app0` / `app1`)
- **Déclencheur** : connexion Wi-Fi temporaire à un point d'accès connu (ex. : hotspot admin)
- **Rollback** : si le nouveau firmware échoue au boot, retour automatique à la version précédente
- **Vérification** : le firmware OTA est signé avec la **clé du device maître** (Ed25519). Seuls les firmwares signés par une clé maître connue sont acceptés

### 4.12 Tâches FreeRTOS

| Tâche | Priorité | Stack | Rôle |
|---|---|---|---|
| `ui_task` | Normale (5) | 8 KB | Rendu LVGL, gestion tactile, rafraîchissement écran |
| `espnow_task` | Haute (7) | 4 KB | Réception/envoi ESP-NOW, discover, paiement, ACK |
| `lora_task` | Normale (5) | 4 KB | Sync périodique (2 min), fragmentation, réception |
| `core_task` | Haute (6) | 8 KB | Traitement DAG, validation TX, checkpoints, wallet |

- **Communication inter-tâches** : queues FreeRTOS (ex. : `espnow_task` → queue → `core_task`)
- **Mutex DAG** : mutex récursif FreeRTOS intégré dans `dag_t`, protège toutes les fonctions publiques (insert, merge, prune, get, count)
- **Mutex état** : `state_mutex` protège les données partagées core↔UI (peers, feedback paiement, broadcast pending)
- **Spinlock LoRa** : `portMUX_TYPE` sur le contexte de fragmentation `s_frag_ctx`
- `ui_task` appelle `lv_timer_handler()` toutes les ~10ms
- `lora_task` est en sommeil entre les cycles de sync (timer 2 min)

### 4.13 Gestion du temps — Module time_manager

Le système supporte deux modes de gestion du temps, configurables au démarrage :

**Mode 1 — Horloge logique Lamport (défaut)**
- Les timestamps des transactions utilisent un compteur Lamport monotone
- À la création d'une TX : `lamport++; tx.timestamp = lamport`
- À la réception d'une TX avec timestamp T : `lamport = max(lamport, T) + 1`
- Les timeouts de verrous (30s) et de fragments (10s) utilisent l'horloge monotonique locale (`esp_timer`)
- Aucune dépendance à une horloge RTC ou un maître

**Mode 2 — Temps maître via LoRa**
- Les devices maîtres diffusent leur timestamp RTC dans les paquets LoRa (message `LORA_TIME_SYNC`, type `0x12`)
- Format du message : `[0x12][pubkey:32][sig:64][timestamp:8][lamport:8]` = 113 octets (signé Ed25519, vérifié à la réception)
- Les devices normaux adoptent le temps du maître : `offset = master_time - local_monotonic`
- Garde-fous :
  - Rejet si delta > 1 heure (maître suspect / RTC défaillant)
  - Multi-maître : adoption du maître avec le plus petit delta
  - Le compteur Lamport interne reste maintenu en parallèle (monotone croissant)
  - Fallback automatique vers Mode 1 si aucun maître entendu depuis 10 minutes
  - Le timestamp retourné ne descend jamais sous la valeur Lamport courante

**Architecture**
- Le module `time_manager` fournit deux fonctions :
  - `get_tx_timestamp()` : timestamp d'ordonnancement (Lamport ou wall-clock corrigé)
  - `get_monotonic()` : temps monotonique local (pour les timeouts, identique dans les deux modes)
- Injection via function pointers dans `wallet_t.get_time` et les callers de `tx_create_*`
- Le module `core/` ne dépend jamais de `time_manager` au niveau CMake — le câblage se fait dans `main.c`

### 4.14 Configuration de la monnaie — Module currency

Le système supporte une monnaie configurable par réseau/événement. La configuration est partagée par tous les devices d'un même `currency_id` et chargée au démarrage.

**Identité et affichage**
- `currency_id` (`uint32_t`) : identifiant unique de la monnaie (hash SHA-256 tronqué du manifeste). Porté dans chaque transaction pour éviter les collisions entre réseaux voisins.
- `name` (max 32 caractères) : nom lisible ("Crédit Solaire", "Festicoin")
- `symbol` (max 8 caractères) : affichage compact ("☼", "FSC")
- `decimals` (`uint8_t`) : position de la virgule pour l'affichage (2 → 500 affiché "5,00")
- `description` (max 64 caractères) : texte événement affiché à l'écran ("Festival du Soleil 2026")

**Politique monétaire**
- `mint_authorities` : tableau de clés publiques Ed25519 autorisées à signer des MINT. Remplace `master_keys_t` dans `tx_validate`.
- `max_supply` (`uint64_t`) : plafond global de création. Les MINT qui feraient dépasser ce plafond sont rejetés. Suivi via un compteur `total_minted` dans le wallet.
- `valid_until` (`uint64_t`, 0 = pas d'expiration) : après cette limite, les nouvelles TX sont rejetées. N'est fiable qu'en mode TIME_MODE_MASTER.
- `initial_balance` (`uint32_t`, défaut 0) : solde crédité au premier boot de chaque device, sans nécessiter de TX MINT. Simplifie le déploiement festival.

**Règles de transaction**
- `min_transfer_amount` (`uint32_t`) : montant minimum par TRANSFER (anti-spam)
- `max_transfer_amount` (`uint32_t`) : plafond par TRANSFER (anti-erreur)
- `transfer_fee` (`uint32_t`, défaut 0) : frais par transfert, **redirigés vers le premier mint_authority** (commission pour l'organisateur). Si aucun `fee_recipient` n'est configuré, les frais sont brûlés (destruction). Stocké dans chaque `transaction_t` (clé CBOR 11) pour conserver l'historique exact des frais appliqués au moment de la TX
- `transfer_cooldown_ms` (`uint32_t`, défaut 0) : délai minimum entre deux TRANSFER émis par le même device (anti-spam temporel)
- Ordre de vérification : currency_id → valid_until → cooldown → min/max amount → solde ≥ amount + fee

**Monnaie fondante (optionnel)**
- `melt_enabled` (`bool`, défaut false) : active la fonte périodique
- `melt_period_seconds` (`uint32_t`) : intervalle entre deux applications (ex: 86400 = 1 jour)
- `melt_volume_mode` : `MELT_MODE_BPS` (pourcentage en basis points) ou `MELT_MODE_FIXED` (montant fixe)
- `melt_bps` (`uint16_t`) : pourcentage du solde retiré par tick (100 = 1,00%)
- `melt_fixed_amount` (`uint32_t`) : montant fixe retiré par tick
- La fonte est un **ajustement local du wallet** (pas de TX MELT dans le DAG) : même formule partout → même résultat
- Activable **uniquement en mode TIME_MODE_MASTER** (horloge fiable requise)
- Le wallet stocke un `last_melt_timestamp` pour le rattrapage des ticks manqués (device éteint)
- Les unités fondues sont **détruites** (sortent de la circulation)

**Architecture**
- Composant `components/currency/` : fournit `currency_config_t`, `currency_rules`, `currency_melt`
- Le `currency_id` est ajouté à `transaction_t` (clé CBOR 10, coûte ~6 octets en CBOR)
- La validation currency s'insère entre `tx_validate_structure` et `tx_validate_signature`
- **Attention** : `currency_validate()` est une validation partielle — l'appelant doit vérifier `mint_authority` séparément via `currency_check_mint_authority()`
- Le `core/` ne dépend PAS de `currency/` au niveau CMake — injection via config dans `main.c`

### 4.15 Broadcast de messages maître

Le device maître peut envoyer un message texte signé à tous les devices à portée LoRa. Ce mécanisme sert à diffuser des annonces (ex : "Fermeture à 18h", "Rechargement disponible au stand B").

**Format du message LoRa**
- Type : `LORA_BROADCAST` (`0x13`)
- Format wire : `[0x13][pubkey:32][sig:64][text_len:1][text:N]`
- Taille : 98 + N octets (min 99, max 255)
- Texte max : 157 caractères (255 - 1 type - 32 pubkey - 64 sig - 1 text_len)
- La signature Ed25519 couvre `[text_len:1][text:N]`

**Envoi (device maître)**
1. L'admin compose le texte via l'UI (alerte visuelle à partir de 128 caractères)
2. Le texte est signé avec la clé privée du device (`crypto_sign`)
3. Le message est packé (`comm_msg_pack_broadcast`) et envoyé via LoRa

**Réception (tous les devices)**
1. `lora_sync_handle_rx()` décode le message (`comm_msg_unpack_broadcast`)
2. Un événement `COMM_EVT_BROADCAST_RECEIVED` est posté sur la queue
3. `core_task` vérifie dans le cache des messages vus (anti-boucle) → si déjà vu, ignore
4. `core_task` vérifie :
   - L'émetteur est dans `mint_authorities` (maître autorisé)
   - La signature Ed25519 est valide (`crypto_verify`)
5. Si valide : le message est stocké, un flag est activé pour l'UI, et le relay est préparé
6. Si invalide : le message est silencieusement ignoré (log warning)

**Relay LoRa (propagation multi-hop)**
- Chaque device qui valide un broadcast le retransmet une fois via LoRa, étendant la portée au-delà de la couverture directe du maître
- Le message relayé est **identique** à l'original (même pubkey + signature du maître) — les récepteurs ne distinguent pas un relay d'un envoi direct
- Un **cache circulaire** de 16 signatures empêche les boucles : un message déjà vu n'est ni re-traité ni re-relayé
- Un **délai aléatoire** (200-1000ms) avant retransmission évite les collisions quand plusieurs devices relayent simultanément
- Le relay s'effectue hors mutex (après `xSemaphoreGive`) pour ne pas bloquer le traitement des événements

**Contraintes**
- Seul un device présent dans `mint_authorities` peut émettre un broadcast original
- Un message avec une signature invalide ou un émetteur inconnu est ignoré (pas de relay)
- Le relay est transparent : un device relaye sans modifier le message
- L'UI affiche le message et réveille l'écran automatiquement (écran `UI_SCREEN_BROADCAST`)

### 4.16 Ping/Pong LoRa — Découverte des devices

Le device maître peut envoyer un ping LoRa pour identifier tous les devices à portée. Utile pour vérifier la couverture réseau et l'état du parc de devices.

**Format des messages LoRa**
- `LORA_PING (0x14)` : `[0x14][pubkey:32][sig:64][ping_id:2 BE]` = 99 octets (signé Ed25519)
- `LORA_PONG (0x15)` : `[0x15][ping_id:2 BE][device_pubkey:32][alias_len:1][alias:N]` = 36 + N octets

**Envoi (device maître)**
1. L'admin déclenche un ping via l'UI
2. `ping_send()` incrémente le `ping_id`, réinitialise les résultats, et envoie le PING via LoRa
3. La collecte de PONGs est activée pour la session courante

**Réception du PING (tous les devices)**
1. `lora_sync_handle_rx()` décode le PING → événement `COMM_EVT_PING_RECEIVED`
2. `core_task` vérifie le seen cache (anti-boucle) → si déjà vu, ignore
3. Ignore si c'est notre propre PING
4. Prépare un PONG avec l'identité du device (pubkey + alias), envoyé de manière **asynchrone** après un délai aléatoire (1-5s) hors mutex
5. Prépare le relay du PING (single-hop, délai 200-1000ms)

**Réception du PONG (device maître)**
1. `lora_sync_handle_rx()` décode le PONG → événement `COMM_EVT_PONG_RECEIVED`
2. `core_task` vérifie que le `ping_id` correspond à la session active
3. Ajoute le device aux résultats (dédoublonné par pubkey, max 32 résultats)

**Relay**
- Le PING est relayé single-hop (même mécanisme que les broadcasts : seen cache + délai aléatoire)
- Les PONGs ne sont **PAS relayés** pour éviter la multiplication du trafic (N PONGs × N relays)
- Les devices à portée directe ou 1-hop du maître sont découverts

**Contraintes**
- `ping_id` (uint16_t) identifie chaque session — les PONGs d'anciennes sessions sont ignorés
- Le délai de réponse PONG (1-5s) étale les transmissions sur le canal LoRa
- Max 32 devices dans les résultats d'un ping

### 4.17 Code PIN — Protection des actions sensibles

Un code PIN à 4 chiffres protège les actions qui engagent le solde du device.

**Stockage**
- Le PIN est hashé via **PBKDF2-HMAC-SHA256** (10 000 itérations) avec un **sel aléatoire de 16 octets**
- Le sel est généré via `esp_fill_random()` à la création du PIN et stocké en NVS (clé `pin_salt`)
- Le hash résultant est stocké en NVS (clé `pin_hash`)
- Le PIN en clair n'est jamais persisté
- Choisi obligatoirement au premier boot (pas de PIN par défaut)

**Blacklist de PIN faibles** (35 patterns interdits)
- Répétitions : 0000, 1111, ..., 9999
- Suites croissantes : 0123, 1234, 2345, ..., 6789
- Suites décroissantes : 9876, 8765, ..., 3210
- Années courantes : 2024, 2025, 2026, 2000, 1999
- Patterns pavé numérique : 1357, 2468, 0852

**Actions protégées**
- Payer (initiate_payment)
- Créer des crédits / MINT (tx_create_mint)
- Modifier l'alias du device
- Modifier le PIN lui-même

**Actions libres** (consultation sans PIN)
- Voir le solde, l'historique, les paramètres (lecture seule)
- Recevoir un paiement ou un broadcast
- Scanner les devices (ping)
- Envoyer un broadcast texte

**Protection anti brute-force — délai progressif**

| Échecs consécutifs | Délai                |
|--------------------|----------------------|
| 1-2                | Aucun                |
| 3                  | 30 secondes          |
| 5                  | 5 minutes            |
| 10                 | Blocage total        |

- Le compteur d'échecs est persisté en NVS (survit au redémarrage)
- Un PIN correct remet le compteur à 0
- En cas de blocage total, un reset NVS est nécessaire (la keypair Ed25519 est conservée)
- **Protection anti-effacement** : après un `nvs_flash_erase()`, le compteur d'échecs est automatiquement initialisé à la valeur de blocage (10), empêchant le brute-force par effacement NVS répété

### 4.18 Renommage distant — LORA_SET_ALIAS

Le device maître peut renommer un device client à distance via LoRa. Cette fonctionnalité est essentielle pour les devices à petit écran (Waveshare 1.47") qui ne disposent pas de clavier pour saisir un alias manuellement.

**Format du message LoRa**

```
COMM_MSG_LORA_SET_ALIAS (0x16)
[type:1][master_pubkey:32][sig:64][target_pubkey:32][alias_len:1][alias:N]
= 130 + N octets (max N=32)
```

- La signature Ed25519 couvre `[target_pubkey:32][alias_len:1][alias:N]`
- Seul un maître reconnu (pubkey dans `mint_authorities`) peut renommer un device
- Le device cible vérifie que `target_pubkey` correspond à sa propre clé publique

**Envoi (device maître uniquement)**

1. L'administrateur sélectionne un device dans la liste des résultats de ping
2. Il saisit le nouvel alias (max 32 caractères)
3. Le maître signe `[target_pubkey][alias_len][alias]` avec sa clé Ed25519
4. Le message est envoyé via LoRa (pas de relay — message ciblé)

**Réception (tous les devices)**

1. `lora_sync_handle_rx()` décode le message → événement `COMM_EVT_SET_ALIAS_RECEIVED`
2. `core_task` vérifie :
   - `master_pubkey` est dans `mint_authorities`
   - La signature est valide
   - `target_pubkey` correspond à la clé publique du device
3. Si toutes les vérifications passent :
   - L'alias est mis à jour en RAM et persisté en NVS
   - Un log confirme le changement

**Sécurité**
- Le message est signé → impossible de forger un renommage
- Le message n'est PAS relayé (pas de single-hop) : le maître doit être à portée LoRa directe du device cible
- Un device ne peut pas se renommer lui-même via ce mécanisme (uniquement via les paramètres locaux avec PIN)

### 4.19 Auto-forward bénéficiaire — Mode encaissement

Permet de configurer un device pour qu'il transfère périodiquement la totalité de son solde vers un bénéficiaire. Cas d'usage principal : festival ou événement où plusieurs stands doivent centraliser les recettes sur un même compte.

**Configuration via LoRa (maître uniquement)**

```
COMM_MSG_LORA_SET_BENEFICIARY (0x17)
[type:1][master_pubkey:32][sig:64][target_pubkey:32][beneficiary_pubkey:32][interval:2 BE]
= 163 octets (taille fixe)
```

- La signature Ed25519 couvre `[target_key:32][beneficiary_key:32][interval:2 BE]`
- `forward_interval_min` : intervalle en minutes entre chaque forward (plancher : 1 min)
- Si `beneficiary_key` est all-zeros : désactivation du mode
- Persisté en NVS (`beneficiary` + `fwd_intv`) — survit au redémarrage

**Forward automatique**

1. À chaque cycle (`forward_interval_min` minutes), le device vérifie son solde
2. Si `solde_disponible > frais_de_transfert`, il crée une TX TRANSFER pour `solde - frais`
3. La TX est auto-confirmée (status = CONFIRMED, pas d'ACK ESP-NOW nécessaire)
4. La TX est insérée dans le DAG et propagée au réseau via la LoRa sync
5. Si le solde est insuffisant pour couvrir les frais, le forward est reporté au prochain cycle

**Avantages du batch forward vs forward immédiat**
- 50 paiements/heure → 1 seule TX de forward au lieu de 50
- Charge DAG quasi-nulle (~1 TX/heure par stand)
- Le bénéficiaire n'a pas besoin d'être à portée ESP-NOW

**Limites**
- Entre deux forwards, le solde est temporairement sur le device du stand
- Un résidu non-transférable peut subsister si `solde restant < min_transfer_amount`
- Le montant maximum par forward respecte `max_transfer_amount` si défini

### 4.20 Sécurité — Durcissement implémenté

**Validation des transactions réseau (`dag_merge`)**
- Toute transaction reçue via LoRa ou ESP-NOW est **validée avant insertion** dans le DAG :
  1. `tx_validate_structure()` — invariants de base (montant > 0, clés non nulles, etc.)
  2. `tx_validate_signature()` — hash SHA-256 + signature Ed25519 (TRANSFER **et** MINT)
  3. `tx_validate_master()` — pour les MINT : vérification que `tx->from` est dans `mint_authorities`
- Les TX invalides sont rejetées avec `DAG_MERGE_REJECTED`

**Désérialisation CBOR durcie**
- Le champ `status` reçu du réseau est **ignoré** : forcé à `TX_STATUS_LOCKED` (TRANSFER) ou `TX_STATUS_CONFIRMED` (MINT). Empêche un attaquant de court-circuiter le mécanisme d'ACK
- Les champs `amount` et `currency_id` sont vérifiés contre `UINT32_MAX` avant troncature (anti-overflow `uint64→uint32`)

**Protection overflow wallet**
- Vérification d'overflow avant toute accumulation de solde (`balance + amount > UINT32_MAX`)
- Accumulateur `uint64_t` pour le calcul du total verrouillé
- Garde `melt_bps > 10000` (retourne 0) et `melt_bps == 0` (retourne balance sans boucle)

**Rate-limiting ESP-NOW**
- Maximum 10 messages par seconde par fenêtre glissante. Au-delà, les paquets sont ignorés (anti-DoS)
- La MAC source des `TX_LOCKED` est conservée dans l'événement pour permettre l'envoi de l'ACK au bon pair
- Le `TX_ACK` est signé Ed25519 avec nonce anti-rejeu : `[0x04][sender_pubkey:32][sig:64][nonce:4][tx_id:32]`

**Communication LoRa**
- Expiration des fragments activée (`lora_frag_expire()` appelée à chaque réception)
- Spinlock ISR-safe sur le contexte de fragmentation

**PONG asynchrone**
- La réponse PONG est différée (flag + timer) au lieu de bloquer sous mutex avec `vTaskDelay`. Élimine le gel de l'UI de 1-5s par PING reçu

**Sécurité — Changements architecturaux implémentés**
- [x] Signer les messages ANNOUNCE / TX_ACK avec la clé Ed25519 de l'émetteur (anti-spoofing) — nonce aléatoire 4 octets + cache 32 entrées anti-rejeu dans `espnow.c`
- [x] Ajouter un nonce/compteur anti-rejeu dans chaque message signé — cache circulaire 32 entrées dans ESP-NOW, vérification à la réception
- [x] Signer les messages TIME_SYNC / PING avec la clé maître (anti-injection temporelle) — format étendu à 113/99 octets dans `lora_sync.c`
- [x] Stocker le `transfer_fee` dans la structure `transaction_t` (CBOR key 11) — plus de paramètre rétroactif dans wallet/checkpoint
- [x] Rediriger les frais vers le premier `mint_authority` (commission organisateur) au lieu de les brûler — la fonte reste détruite
- [x] Activer le chiffrement flash ESP32 pour protéger la clé privée en NVS — AES-256 mode dev + NVS encryption via `nvs_sec_provider`

**Sécurité — Restant (production)**
- [ ] Secure Boot V2 (RSA-3072) — documenté dans sdkconfig, nécessite eFuse irréversible + hardware ECO3+ pour ESP32
- [ ] Whitelist DISCOVER [M6] — ne répondre au DISCOVER qu'aux peers autorisés (renforcement optionnel)

### 4.21 Support multi-écran — CYD + Waveshare

Le système supporte deux form factors avec adaptation automatique de l'UI :

| | CYD (ESP32) | Waveshare 1.47" (ESP32-S3) |
|---|---|---|
| Écran | ILI9341, 320×240, portrait | JD9853, 320×172, paysage (MADCTL 0x60) |
| Tactile | XPT2046 résistif (SPI) | AXS5106L capacitif (I²C) |
| Détection | `min(w,h) >= 200` → grand | `min(w,h) < 200` → petit |
| PIN | Pavé numérique 3×4 | Style Ledger horizontal (◄ digit ► + Valider) |
| Home | 4 boutons colonne | 4 boutons ligne horizontale |
| Admin | Colonne, 5 boutons | Grille 2×3 (broadcast et rename désactivés) |
| Receive | Carte 160×160 | Carte 290×80 |
| Settings | Colonne | Grille 2 colonnes |

- Le petit écran (Waveshare) **reçoit et relaye** les broadcasts mais ne peut pas en **composer** (pas de clavier)
- Le renommage distant n'est pas accessible depuis le petit écran (même raison)
- L'écran Forward est fonctionnel sur les deux tailles (rollers uniquement, pas de clavier)

## 5. Contraintes ESP32

| Ressource | Limite | Impact |
|---|---|---|
| RAM | ~520 KB | Élagage du DAG nécessaire — ne garder que les N dernières transactions + tips |
| Flash | 4 MB (CYD) | Stockage persistant du DAG et des clés — espace limité, élagage important |
| ESP-NOW payload | 250 octets | Transaction compacte — sérialisation CBOR |
| LoRa payload | 255 octets | Sync par deltas, pas de transfert de DAG complet |
| ESP-NOW peers | 20 max (chiffré) | Suffisant pour le paiement de proximité |

## 6. Questions ouvertes

### Hardware
- [ ] Pinout UART du Wio-E5 sur le CYD (TX/RX)
- [ ] Sécurité physique du device (anti-tamper ?)
- [ ] Board finale (après prototype CYD)

### Logiciel
- [ ] Gestion de la liste des clés maîtres autorisées (comment la distribuer / mettre à jour ?)
- [ ] Protocole de réconciliation quand deux maîtres ont MINT des montants incohérents
- [ ] Politique de rétention des checkpoints en Flash (combien garder ?)
- [x] Gestion du temps → voir section 4.13 (Lamport + Temps Maître configurables)
- [ ] **Fin de monnaie** — mécanisme pour geler/terminer une monnaie. Options envisagées : expiration programmée (timestamp dans currency_config) + arrêt anticipé par multi-sig (2/N maîtres). Doit aussi couvrir la prolongation. Message `LORA_CURRENCY_END` à concevoir
- [x] **Auto-forward bénéficiaire** → voir section 4.19 (batch forward périodique via LORA_SET_BENEFICIARY)
- [ ] **[C5] Manifeste de monnaie signé (reporté)** — Actuellement `init_currency_config()` en main.c est hardcodé et chaque device devient son propre mint_authority. Pour un vrai réseau partagé, il faut :
  - Définir une struct `currency_manifest_t` = `currency_config_t` + `manifest_version` + `manifest_signature` (Ed25519 d'une root key de fondation)
  - Persister le manifeste en NVS (blob signé), charger au boot
  - Si absent → UI "setup réseau" : créer une nouvelle monnaie (root locale) OU rejoindre une monnaie existante (scan QR de la root key + réception LoRa du manifeste)
  - Nouveau message `COMM_MSG_LORA_MANIFEST` pour diffuser le manifeste aux nouveaux pairs
  - Vérifier la signature du manifeste au boot ; rejeter s'il ne matche pas la root key connue
  - Gérer la mise à jour du manifeste (ex. ajout d'un nouveau mint_authority) via re-signature + version incrémentée
  - **Fallback actuel** : init hardcoded, marqué "prototype / dev" — à remplacer avant tout déploiement terrain
