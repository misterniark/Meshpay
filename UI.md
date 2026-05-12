# Interface utilisateur — Écrans et navigation

## Vue d'ensemble

L'UI est construite avec **LVGL** et gérée par une `ui_task` FreeRTOS dédiée.
Deux profils de device coexistent :

- **Client** : toutes les fonctionnalités de base (paiement, historique, réception de broadcasts)
- **Maître** : fonctionnalités client + administration (MINT, broadcast, ping, config)

La distinction est déterminée au runtime : un device est maître si sa pubkey est dans `s_currency.mint_authorities[]`.

**Sécurité** : un code PIN à 4 chiffres protège les actions sensibles (paiement, MINT, modification de l'alias). Le PIN est choisi au premier boot et stocké hashé (SHA-256) en NVS.

---

## Navigation

```
  PREMIER BOOT
  ┌──────────┐     ┌──────────┐
  │ SETUP    │────>│ SETUP    │────> ACCUEIL
  │  PIN     │     │  ALIAS   │
  │ (4 chif) │     │(optionnel│
  └──────────┘     └──────────┘
```

```
                    ┌──────────────┐
              ┌────>│   ACCUEIL    │<────┐
              │     │  (solde)     │     │
              │     └──┬───┬───┬──┘     │
              │        │   │   │        │
        ┌─────┘   ┌────┘   │   └────┐   └─────┐
        v         v        v        v          v
  ┌──────────┐ ┌───────┐ ┌──────┐ ┌──────┐ ┌──────────┐
  │ PAYER    │ │RECEVOIR│ │HISTO │ │PARAMS│ │  ADMIN   │
  │          │ │(QR/NFC)│ │      │ │      │ │(maître)  │
  └──────────┘ └───────┘ └──────┘ └──────┘ └──────────┘
```

Retour à l'accueil depuis chaque écran via un bouton retour ou swipe.

---

## 1. Écrans client (tous les devices)

### 1.0 Premier boot — Configuration initiale

Affiché une seule fois au tout premier démarrage (détecté via flag NVS `first_boot`).

**Étape 1 — Choix du code PIN**
- Titre : "Choisissez votre code PIN"
- Pavé numérique (0-9) + affichage 4 points/étoiles
- L'utilisateur saisit 4 chiffres

**Étape 2 — Confirmation du PIN**
- Titre : "Confirmez votre code PIN"
- Nouvelle saisie des 4 chiffres
- Si différent de l'étape 1 → message d'erreur, retour étape 1
- Si identique → le hash SHA-256 du PIN est sauvegardé en NVS

**Étape 3 — Alias (optionnel)**
- Affiche l'alias auto-généré (ex: "Brave-Loup")
- Titre : "Nom de votre device"
- Bouton "Garder" → conserve l'alias généré
- Bouton "Modifier" → clavier LVGL pour saisir un alias custom (max 32 chars)

Après ces étapes → redirection vers l'écran d'accueil.

---

### 1.1 Accueil / Solde

Écran principal affiché au démarrage.

| Élément             | Source                                                        |
|---------------------|---------------------------------------------------------------|
| Alias du device     | `s_device_alias`                                              |
| Solde disponible    | `wallet_get_balance()` formaté avec `s_currency.decimals`     |
| Symbole monnaie     | `s_currency.symbol`                                           |
| Nom monnaie         | `s_currency.name`                                             |
| Description event   | `s_currency.description`                                      |
| Indicateur maître   | Visible si device est dans `mint_authorities`                 |

**Boutons de navigation** :
- Payer
- Historique
- Paramètres
- Admin (visible uniquement si maître)

**Comportement** :
- Le solde se rafraîchit à chaque retour sur cet écran
- Si `s_broadcast_pending == true` → interruption : afficher l'écran 1.6 (notification broadcast)

---

### 1.2 Payer

Écran d'envoi de paiement en plusieurs étapes.

**Étape 0 — Saisie du PIN**
- Pavé numérique 4 chiffres (voir section 3.7)
- Si PIN incorrect → message d'erreur + délai progressif (voir section 3.7)
- Si PIN correct → passage à l'étape 1

**Étape 1 — Montant**
- Champ numérique (pavé tactile ou clavier numérique LVGL)
- Affichage du solde disponible en référence
- Validation : `min_transfer_amount` ≤ montant ≤ `max_transfer_amount`
- Si `transfer_fee > 0` : afficher "Frais : X {symbole}" sous le montant
- Bouton "Suivant" → passe à l'étape 2

**Étape 2 — Sélection du destinataire**
- Lancement automatique de la découverte ESP-NOW (broadcast DISCOVER)
- Liste des peers découverts : alias + 4 derniers hex de la pubkey (ex: "Brave-Loup [A3F2]")
- Animation de recherche pendant la découverte
- Timeout de découverte : ~15 secondes
- Sélection d'un peer → étape 3

**Étape 3 — Confirmation**
- Récapitulatif : montant, frais, destinataire (alias), total débité
- Bouton "Confirmer" → appel `initiate_payment(to, amount)`
- Bouton "Annuler" → retour accueil

**Étape 4 — Résultat**
- Spinner pendant l'attente de l'ACK (max 30s, timeout verrouillage)
- Succès : animation + "Paiement confirmé"
- Échec : message d'erreur (solde insuffisant, timeout, etc.)
- Bouton "Retour" → accueil

---

### 1.3 Historique des transactions

Liste scrollable des transactions du device (envoyées et reçues).

| Colonne       | Source                                                              |
|---------------|---------------------------------------------------------------------|
| Type          | `tx.type` — icône flèche ↑ (envoyé) ou ↓ (reçu)                   |
| Montant       | `tx.amount` formaté avec `s_currency.decimals` + `s_currency.symbol`|
| Correspondant | Alias du peer (via `s_peers[]`) ou 4 hex de la pubkey si inconnu   |
| Statut        | `tx.status` — LOCKED (⏳), CONFIRMED (✓), CANCELLED (✗)           |
| Timestamp     | `tx.timestamp` — relatif ("il y a 5 min") ou absolu                |

**Filtrage** :
- Parcours de `s_dag.transactions[0..count-1]`
- Afficher uniquement les TX où `tx.from == own_pubkey` ou `tx.to == own_pubkey`
- Tri par timestamp décroissant (plus récente en haut)

**Détail d'une TX** (tap sur une ligne) :
- ID de la TX (hash tronqué)
- Montant exact + frais
- Pubkey complète du correspondant
- Statut détaillé
- Timestamp exact

---

### 1.4 Paramètres

Configuration locale du device.

| Paramètre           | Source / Action                                          | Modifiable          |
|----------------------|----------------------------------------------------------|---------------------|
| Alias du device      | `s_device_alias` — sauvegardé en NVS                    | Oui (voir ci-dessous) |
| Clé publique         | `s_keypair.public_key` — affichage hex ou base64         | Non                 |
| Monnaie              | `s_currency.name` / `symbol` / `description`             | Non                 |
| Frais de transfert   | `s_currency.transfer_fee`                                | Non                 |
| Version firmware     | Macro de build                                           | Non                 |

**Modification de l'alias** :

*ESP32 CYD (écran 2.8")* :
- Saisie du PIN requise avant modification (voir section 3.7)
- Clavier LVGL pour saisie (max 32 caractères)
- Sauvegarde via `s_storage.blob_write(NVS_NAMESPACE, NVS_KEY_ALIAS, ...)`
- Mise à jour de `s_device_alias` et `s_device_alias_len` en mémoire

*ESP32-S3 Waveshare (écran 1.47")* :
- Pas de saisie texte sur ce device (écran trop petit pour un clavier)
- L'alias est modifié à distance par un device maître via `LORA_SET_ALIAS` (voir section 2.5)
- L'écran Paramètres affiche l'alias en lecture seule

**Modification du PIN** :
- Saisie de l'ancien PIN requis (méthode adaptée au hardware, voir section 3.7)
- Saisie du nouveau PIN (4 chiffres) + confirmation
- Sauvegarde du nouveau hash en NVS

---

### 1.5 Réception de paiement

Écran d'attente passive de paiement entrant (optionnel, peut être implicite).

- Affiche un identifiant visuel du device (alias + pubkey courte)
- Notification quand un paiement est reçu :
  - Montant, expéditeur (alias)
  - Animation de confirmation

---

### 1.6 Notification de broadcast maître

Écran modal déclenché quand `s_broadcast_pending == true`.

| Élément          | Source                                      |
|------------------|---------------------------------------------|
| Texte du message | `s_pending_broadcast.text`                  |
| Longueur         | `s_pending_broadcast.text_len` (max 157)    |

**Comportement** :
- Allumer l'écran (backlight ON) si éteint
- Afficher le message en plein écran avec une police lisible, faire défiler pour les petits écran
- Bouton "OK" / "Fermer" pour revenir à l'écran précédent
- Remettre `s_broadcast_pending = false` à la fermeture
- Si le device était en veille, retourner à l'écran d'accueil après fermeture

---

## 2. Écrans maître (master uniquement)

Ces écrans ne sont accessibles que si le device est dans `mint_authorities[]`.

### 2.1 Menu admin

Point d'entrée vers les fonctionnalités maître.

**Boutons** :
- Créer des crédits (MINT)
- Envoyer un message (broadcast)
- Scanner les devices (ping/pong) — inclut le renommage à distance

---

### 2.2 Créer des crédits (MINT)

Création de crédits et distribution à un device.

**Étape 0 — Saisie du PIN** (identique à l'écran Payer, voir section 3.7)

**Étape 1 — Paramètres**
- Champ montant à créditer
- Sélection du destinataire :
  - Liste des peers connus (`s_peers[]`)
  - Ou lancement d'un ping pour découvrir les devices
- Validation : montant > 0, `total_minted + amount ≤ max_supply` (si max_supply > 0)

**Étape 2 — Confirmation**
- Récapitulatif : montant, destinataire
- Avertissement : "Cette action crée de la monnaie"
- Bouton "Confirmer" → création TX MINT via `tx_create_mint()`

**Étape 3 — Résultat**
- Succès / échec

---

### 2.3 Envoyer un message (broadcast texte)

Composition et envoi d'un message texte à tous les devices LoRa.

| Élément             | Détail                                                    |
|---------------------|-----------------------------------------------------------|
| Zone de saisie      | Champ texte multiligne, clavier LVGL                      |
| Compteur caractères | `N / 157` — affichage dynamique                          |
| Alerte visuelle     | Couleur orange à partir de 128 caractères                 |
| Bouton "Envoyer"    | Appel `broadcast_text_send(text, text_len)`               |

**Comportement** :
- Blocage du bouton "Envoyer" si texte vide ou > 157 caractères
- Feedback après envoi : "Message envoyé" ou erreur
- Retour au menu admin

---

### 2.4 Scanner les devices (ping/pong)

Découverte des devices à portée LoRa.

**Étape 1 — Lancement**
- Bouton "Scanner" → appel `ping_send()`
- Animation de recherche (spinner)
- Attente de ~10 secondes pour collecter les PONGs

**Étape 2 — Résultats**
- Liste des devices découverts :

| Colonne     | Source                                         |
|-------------|------------------------------------------------|
| Alias       | `s_ping_results[i].alias`                      |
| Pubkey      | `s_ping_results[i].key` (4 derniers hex)       |

- Compteur : `s_ping_result_count` / MAX_PING_RESULTS
- Bouton "Rescanner" pour relancer
- Bouton "Renommer" sur un device sélectionné → écran 2.5
- Bouton "Retour" → menu admin

---

### 2.5 Renommer un device (SET_ALIAS à distance)

Permet au maître de modifier l'alias d'un device client via LoRa. Particulièrement utile pour les devices à petit écran (Waveshare) qui n'ont pas de clavier.

**Accès** : depuis les résultats du scan (écran 2.4), sélectionner un device → "Renommer"

**Étape 1 — Saisie du nouvel alias**
- Affichage de l'alias actuel du device
- Clavier LVGL pour saisir le nouvel alias (max 32 caractères)

**Étape 2 — Confirmation**
- Récapitulatif : device cible (alias + pubkey courte), nouvel alias
- Bouton "Confirmer" → envoi `LORA_SET_ALIAS` signé

**Comportement** :
- Le message `LORA_SET_ALIAS (0x16)` contient : pubkey du maître, signature, pubkey cible, nouvel alias
- Le device cible vérifie que l'émetteur est un maître autorisé (`mint_authorities`)
- Si valide → l'alias est mis à jour en NVS et en mémoire
- Feedback : "Alias envoyé" (pas de confirmation de réception — LoRa est unidirectionnel)

---

## 3. Éléments transversaux

### 3.1 Barre de statut (header)

Présente sur tous les écrans :
- Alias du device (tronqué si trop long)
- Solde rapide (petit format)
- Indicateur maître (icône étoile ou couronne)

### 3.2 Veille / économie d'énergie

- Extinction du backlight après N secondes d'inactivité
- Réveil au toucher ou à la réception d'un broadcast (`s_broadcast_pending`)
- Le touch controller doit rester actif en veille pour détecter le réveil

### 3.3 Feedback visuel paiement

- LED RGB (si disponible sur le hardware) :
  - Bleu clignotant : en attente d'ACK
  - Vert : paiement confirmé
  - Rouge : paiement échoué / annulé

### 3.4 Formatage des montants

- Utiliser `s_currency.decimals` pour placer la virgule
- Exemple avec `decimals = 2` : valeur interne `1500` → affichage `15,00`
- Toujours suffixer avec `s_currency.symbol`
- Séparateur décimal : virgule (`,`) — convention locale

### 3.5 Affichage des pubkeys

- Format court pour l'UI : 4 derniers octets en hex majuscule (ex: `[A3F2B1C8]`)
- Format complet dans les écrans de détail : hex avec espaces tous les 4 chars
- Jamais afficher la clé privée

### 3.6 Thread safety

Tout accès aux variables globales (`s_wallet`, `s_dag`, `s_peers`, etc.) depuis `ui_task` doit passer par `s_state_mutex` avec un timeout de 1000ms. En cas de timeout, afficher un indicateur "occupé" et réessayer.

### 3.7 Code PIN

**Stockage** :
- Le PIN est hashé en SHA-256 avant stockage en NVS (clé `pin_hash`)
- Le PIN en clair n'est jamais stocké ni conservé en mémoire après vérification

**Saisie — variante selon l'écran** :

*ESP32 CYD (320×240, écran 2.8")* :
- Pavé numérique 0-9 avec 4 emplacements visuels (points `●` / étoiles `*`)
- Bouton d'effacement (supprimer le dernier chiffre)
- Bouton d'annulation (retour à l'écran précédent)
- Affichage du bouton validé ou retour à la saisie automatique dès le 4e chiffre saisi

*ESP32-S3 Waveshare (172×320, écran 1.47")* :
- Méthode style Ledger adaptée au petit écran
- Affichage : 4 emplacements `_ _ _ _`, le chiffre courant affiché en grand au centre
- Zone tactile gauche (moitié gauche de l'écran) : décrémenter le chiffre (9→8→7...→0→9)
- Zone tactile droite (moitié droite de l'écran) : incrémenter le chiffre (0→1→2...→9→0)
- Appui simultané gauche+droite : valider le chiffre et passer au suivant
- Indicateurs visuels : flèches `◄` et `►` sur les bords de l'écran
- Pas de clavier — seulement 2 zones de toucher sur toute la hauteur de l'écran

**Vérification** :
- Hash SHA-256 de la saisie comparé au hash stocké en NVS
- Si correct → action autorisée
- Si incorrect → message "Code incorrect", compteur d'échecs incrémenté

**Délai progressif après échecs** :
| Échecs consécutifs | Délai avant nouvelle tentative |
|--------------------|-------------------------------|
| 1-2                | Aucun                         |
| 3                  | 30 secondes                   |
| 5                  | 5 minutes                     |
| 10                 | Blocage total                 |

- Le compteur d'échecs est stocké en NVS (persiste au redémarrage)
- Un PIN correct remet le compteur à 0
- En cas de blocage total, un reset NVS est nécessaire (l'identité crypto du device est conservée)

**Actions protégées par le PIN** :
- Payer (écran 1.2)
- Créer des crédits / MINT (écran 2.2)
- Modifier l'alias (écran 1.4)
- Modifier le PIN (écran 1.4)

**Actions libres (sans PIN)** :
- Consulter le solde (écran 1.1)
- Consulter l'historique (écran 1.3)
- Recevoir un paiement (écran 1.5)
- Voir une notification broadcast (écran 1.6)
- Scanner les devices / ping (écran 2.4)
- Envoyer un message broadcast (écran 2.3)

---

## 4. Résumé des écrans par profil

| #   | Écran                    | Client | Maître | PIN requis |
|-----|--------------------------|--------|--------|------------|
| 1.0 | Setup initial (1er boot) | ✓      | ✓      | —          |
| 1.1 | Accueil / Solde          | ✓      | ✓      | Non        |
| 1.2 | Payer                    | ✓      | ✓      | Oui        |
| 1.3 | Historique               | ✓      | ✓      | Non        |
| 1.4 | Paramètres               | ✓      | ✓      | Partiel    |
| 1.5 | Réception de paiement    | ✓      | ✓      | Non        |
| 1.6 | Notification broadcast   | ✓      | ✓      | Non        |
| 2.1 | Menu admin               |        | ✓      | Non        |
| 2.2 | Créer des crédits (MINT) |        | ✓      | Oui        |
| 2.3 | Envoyer un message       |        | ✓      | Non        |
| 2.4 | Scanner les devices      |        | ✓      | Non        |

**Total : 11 écrans** (7 client + 4 maître)

---

## 5. Adaptation hardware

| Target                  | Écran          | Résolution | Contrainte                              |
|-------------------------|----------------|------------|-----------------------------------------|
| ESP32 CYD               | ILI9341 2.8"   | 320×240    | Référence, tactile résistif XPT2046     |
| ESP32-S3 Waveshare 1.47"| JD9853 1.47"   | 172×320    | Écran petit, tactile capacitif AXS5106L |

- Le layout LVGL doit s'adapter aux deux résolutions
- Sur le Waveshare (172px de large), privilégier une navigation verticale et des éléments plus grands
- Les listes (historique, résultats ping) doivent scroller verticalement sur les deux formats
