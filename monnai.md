# Paramètres de monnaie (crédits)

Ce document décrit les **paramètres configurables** d’une monnaie locale sur le réseau offline. Ils sont distincts du **montant brut** stocké en chaîne (`uint32` dans chaque transaction) : l’affichage et les règles métier s’appuient sur cette configuration.

---

## 1. Identité et représentation (UI)

Ces champs définissent **comment la monnaie est perçue** sur l’écran tactile. Ils n’altèrent pas la valeur stockée en mémoire ; ils servent au **formatage** et au **branding**.

| Champ | Type (proposé) | Rôle |
|--------|----------------|------|
| **name** | chaîne courte (ex. ≤ 32 caractères) | Nom lisible : « Crédit Solaire », « Festicoin », etc. |
| **symbol** | chaîne courte (1 à 3 caractères ou glyphe supporté par la fonte) | Affichage compact à côté des montants : `☼`, `FSC`, etc. |
| **decimals** | `uint8` (ex. 0 à 9, typiquement 2) | **Échelle d’affichage** : le montant en mémoire est un entier ; `decimals` indique où placer la virgule virtuelle. |

**Exemple — `decimals = 2`**

- Valeur stockée en transaction : `500` (uint32).
- Affichage utilisateur : **5,00** `{symbol}` (ou **5.00** selon locale UI).

Formule d’affichage (conceptuelle) :

```
valeur_affichée = amount / 10^decimals
```

Les saisies clavier peuvent être interprétées dans l’autre sens (saisie « 5,00 » → `amount = 500` si `decimals = 2`).

---

## 2. Politique monétaire (sécurité et MINT)

Ces paramètres fixent **à quelle monnaie** le réseau se rattache et **qui peut créer** des unités, avec des **garde-fous** d’émission et de durée de vie.

| Champ | Type (proposé) | Rôle |
|--------|----------------|------|
| **currency_id** | identifiant fixe (ex. `uint32`, ou **8 premiers octets** d’un hash SHA-256 d’un manifeste / nom canonique) | **Sépare les monnaies** : deux événements géographiquement proches qui se croisent ne mélangent pas par erreur les soldes. Toute transaction (MINT ou TRANSFER) doit porter le même `currency_id` que la config locale pour être acceptée. |
| **mint_authorities** | tableau de **clés publiques** Ed25519 (32 octets chacune) | Liste des **devices maître** autorisés à signer des transactions **MINT** pour *cette* monnaie. Cohérent avec la spec « plusieurs maîtres indépendants » ; ici la liste est **par currency_id**. |
| **max_supply** | `uint64` ou `uint32` selon besoin de précision | **Plafond global** de création monétaire (somme des montants MINT confirmés pour ce `currency_id`). Les MINT qui feraient dépasser ce plafond sont **rejetés**. Rassure sur l’absence d’inflation illimitée. |
| **valid_until** | timestamp d’ordonnancement (voir ci-dessous) ou `0` = pas d’expiration | Après cette limite, **nouvelles transactions rejetées** pour cette monnaie (usage type **festival**). Nécessite une notion de temps cohérente sur le device (ex. module **time_manager** : Lamport + option sync maître LoRa). |

**Notes**

- **`valid_until`** : si l’horloge wall-clock n’est pas fiable, on peut soit désactiver (`0`), soit n’activer l’expiration qu’en **mode temps maître** avec garde-fous (delta max, fallback Lamport).
- **`max_supply`** : la somme courante se déduit du DAG / checkpoints ; à préciser en implémentation (compteur maintenu dans le wallet vs recalcul périodique).

---

## Monnaie « fondante » (fonte périodique sur les soldes)

La monnaie peut être configurée pour **réduire automatiquement le solde disponible** au fil du temps (démurrage / incitation à dépenser), via un **paramètre de temps** (période) et un **paramètre de volume perdu** (combien est retiré à chaque échéance).

| Champ | Type (proposé) | Rôle |
|--------|----------------|------|
| **melt_enabled** | `bool` (défaut **false**) | Active ou non la fonte sur les soldes de cette `currency_id`. |
| **melt_period_seconds** | `uint32` (ex. `86400` = 1 jour) | **Temps** entre deux applications de la fonte : à chaque période écoulée, le volume perdu est appliqué une fois (ou plusieurs « ticks » d’un coup si le device était éteint — voir ci-dessous). |
| **melt_volume_mode** | enum : `BPS` \| `FIXED` | **Mode de calcul** du volume retiré à chaque tick de période. |
| **melt_bps** | `uint16` (ex. `100` = 1,00 % par tick) | Si `BPS` : pourcentage du **solde disponible** (hors montants verrouillés) soustrait à chaque période. Plafonner en spec (ex. max `10000` = 100 %) pour éviter les erreurs de configuration. |
| **melt_fixed_amount** | `uint32` | Si `FIXED` : nombre d’unités (même échelle que `amount` en chaîne) retiré à chaque période, typiquement `min(melt_fixed_amount, solde_disponible)`. |

**Horloge** — La fonte suppose une base temporelle : de préférence le **temps maître** (sync LoRa) si le réseau doit rester aligné ; sinon **temps monotonique** local (fonte cohérente sur un device isolé, possible décalage entre devices).

**Effet économique** — Les unités fondues **sortent de la circulation** (destruction), sauf extension ultérieure (ex. redirection vers une caisse).

**Implémentation DAG (piste)** — Soit une transaction **`MELT`** / **`BURN`** périodique par device (traçable à la sync LoRa), soit un **ajustement de wallet** identique sur tous les pairs si la même formule et la même horloge sont garanties.

**Ticks rattrapés** — Si `melt_period_seconds` s’est écoulé plusieurs fois pendant une absence : appliquer **N ticks** consécutifs (N = temps écoulé ÷ période, avec borne max prudente anti-overflow) avant tout nouveau TRANSFER.

---

## 3. Règles de transaction (friction et limites)

Ces seuils **protègent le DAG et la RAM** (fenêtre ~500 TX) et **limitent les erreurs** de saisie.

| Champ | Type (proposé) | Rôle |
|--------|----------------|------|
| **min_transfer_amount** | `uint32` (même unité que `amount` en chaîne) | **Anti-spam** : rejeter les TRANSFER dont `amount < min_transfer_amount`. Réduit le risque de saturer la fenêtre glissante avec des micro-transactions. |
| **max_transfer_amount** | `uint32` | **Plafond par transaction** : aucun TRANSFER ne peut dépasser cette valeur (erreur de frappe, compromission partielle du clavier). |
| **transfer_fee** | `uint32` (défaut **0**) | **Frais par transfert** : montant **déduit en plus** du paiement au bénéficiaire. Politique configurable : **fonte** (unités détruites, sortent de la masse en circulation) ou **taxe** vers un compte réservé (si le modèle le prévoit). Si `0`, comportement actuel sans friction. |

**Ordre de vérification (suggestion)**

1. `currency_id` cohérent avec la config locale.  
2. Si `valid_until` actif : temps courant ≤ `valid_until`.  
3. Pour un TRANSFER : `min_transfer_amount ≤ amount ≤ max_transfer_amount`.  
4. Solde disponible ≥ `amount + transfer_fee` (si les frais sont payés par l’émetteur).  
5. Application du fee selon la politique (destruction vs redirection).  
6. Si **melt_enabled** : appliquer les **ticks de fonte** dus (section *Monnaie « fondante »*) avant de finaliser le solde affiché ou la validation d’un paiement.

---

## Stockage et portée

- Ces paramètres forment une **configuration de monnaie** partagée par tous les devices d’un **même événement / réseau logique** (flashée ou distribuée une fois, puis figée ou mise à jour via procédure admin — *à trancher en prod*).
- Les **transactions** embarquent au minimum **`currency_id`** (et éventuellement une version de schéma) pour éviter les collisions entre communautés.
- Les champs **`melt_*`** font partie de la config partagée : la même politique de temps et de volume doit être utilisée par tous les participants d’une monnaie pour que les soldes restent cohérents après sync.
