---
# MeshPay — Payer sans internet, sans banque, sans serveur

  

## Le problème


Imaginez un marche de village sans connexion internet. Un festival en pleine campagne. Une zone de crise ou les réseaux sont coupes. Comment échanger de la valeur sans espèces, sans terminal bancaire, sans 4G ?

Imaginez un lieu, une ville, une communauté qui veut gagner en souveraineté monétaire : "comment est créée ma monnaie", "quelle règles suit-elle" , "comment la gagne t'on", "dans quoi peut-on la dépenser"

Imaginer un monde où les smartphone deviennent inutilisable faute d'électricité suffisante, faute de réseau fonctionnel ou non censuré.

MeshPay répond a ces questions : c'est un **système de paiement entièrement hors-ligne et décentralisé**, qui fonctionne entre de petits boitiers a écran tactile, sans aucune infrastructure externe. Pas de serveur, pas de cloud, pas d'internet. Juste des appareils qui communiquent directement entre eux par ondes radio. Et avec une consommation minimes qui leurs permet de tenir des jours, voir de semaines.

  

---

  

## A qui ça s'adresse ?

  
- **Des lieux et des communautés** qui veulent s'émanciper du système monétaire et bancaire du capitalisme
-
- **Organisateurs de festivals** qui veulent proposer un système de paiement interne (jetons numériques) sans dépendre d'une connexion internet.

- **Communautés locales** qui souhaitent créer leur propre monnaie d'échange, dans un marche, un village, une coopérative.

- **Zones isolées** (montagnes, zones rurales, pays en developpement) ou les infrastructures bancaires et télécoms sont absentes ou peu fiables.

- **Situations d'urgence** ou les réseaux classiques sont hors service mais ou les échanges doivent continuer.

  

En résumé : toute situation ou des gens ont besoin d'échanger de la valeur (marchande ou non) **sans dépendre d'un tiers**.

  

---

  

## Comment ça marche ? (Vue d'ensemble)

  

Chaque utilisateur possède un petit boitier électronique a faible coût avec un écran tactile. Quand deux personnes veulent faire un échange :

  

1. L'émetteur appuie sur "Payer" et voit les appareils a proximité.

2. Il sélectionne le destinataire, entre le montant, et confirme avec son code PIN.

3. Le paiement est envoye directement a l'autre boitier par onde radio.

4. Le destinataire reçoit et confirme automatiquement.

5. C'est fait. En moins d'une seconde, sans internet.

  

Ensuite, les deux appareils partagent l'information avec le reste du reseau progressivement, de proche en proche, comme une rumeur qui se propage dans un village.

  

---

  

## Les concepts clés, expliqués simplement

  

### Le reseau Mesh (ou reseau maillé)

  

Dans un reseau classique (téléphone, internet), toutes les communications passent par un point central : une antenne-relais, un serveur, un routeur. Si ce point tombe, tout s'arrête. Si une censure s'applique, c'est par ce point centralisé.

  

Un **reseau mesh** fonctionne différemment : chaque appareil est a la fois émetteur et relais. Si Alice veut envoyer un message a David mais qu'ils sont trop éloignés, le message passe par Bob et Charlie, qui le relaient automatiquement. Il n'y a pas de point central, pas de maitre du reseau. Si un appareil s'éteint, les autres trouvent un autre chemin.

  

MeshPay utilise ce principe : chaque boitier est un noeud du reseau qui peut relayer les transactions des autres.

  

### ESP-NOW — Le paiement eclair

  

**ESP-NOW** est un protocole de communication sans fil integre aux puces ESP32 (les microcontroleurs utilises par MeshPay). Il permet a deux appareils d'echanger des donnees **en 1 a 5 millisecondes**, sans avoir besoin de se connecter a un reseau Wi-Fi.

  

C'est la couche de communication rapide et courte portee (~200 metres) de MeshPay. Elle sert a :

- Decouvrir les appareils a proximite

- Envoyer et confirmer un paiement en temps reel

  

Pensez-y comme un talkie-walkie numerique instantane entre deux appareils.

  

### LoRa — La portee longue distance

  

**LoRa** (pour *Long Range*) est une technologie radio basse consommation qui permet de communiquer sur **plusieurs kilometres** (2 km et plus selon le terrain), meme a travers des murs et des collines.

  

MeshPay utilise LoRa pour la **synchronisation du reseau** : toutes les 2 minutes, chaque appareil diffuse ses transactions recentes en LoRa. Les appareils voisins les captent, les verifient, et les rediffusent a leur tour. Ainsi, meme si deux personnes ont fait un echange a l'autre bout du marché, tout le reseau finit par en être informe.

  

LoRa est lent (quelques kilo-octets par seconde) mais fiable sur longue distance. ESP-NOW est rapide mais courte portée. Les deux se complètent parfaitement.

  

### Le DAG — Un registre sans blockchain

  

Vous avez peut-être entendu parler de la **blockchain** : un registre ou chaque transaction est rangée dans un bloc, les blocs formant une chaine linéaire. C'est solide, mais lourd — inadapté a de petits appareils avec peu de mémoire. De plus la consommation énergétique est énorme.

  

MeshPay utilise a la place un **DAG** (*Directed Acyclic Graph*, ou graphe oriente acyclique). Imaginez un arbre généalogique : chaque transaction peut avoir plusieurs "parents" (transactions précédentes dont elle dépend). Cela permet a **plusieurs transactions de coexister en parallèle** sans se bloquer mutuellement.

  

Concrètement :

- Alice paie Bob et Charlie paie David au même instant ? Pas de problème, les deux transactions s'inscrivent en parallèle dans le DAG.

- Quand deux appareils se resynchronisent, ils fusionnent leurs DAG respectifs — comme deux branches d'un arbre qui se rejoignent.

  

Le DAG est plus léger et plus rapide qu'une blockchain, ce qui est essentiel pour fonctionner sur des microcontroleurs avec seulement 520 Ko de mémoire vive.

  

### Les signatures numériques — La confiance sans tiers

  

Comment s'assurer qu'un paiement est authentique si personne ne surveille le reseau ? Grace a la **cryptographie a cle publique**.

  

Chaque appareil possede une paire de cles :

- Une **cle privee** (secrete, jamais partagee) qui sert a "signer" les transactions, comme une empreinte unique.

- Une **cle publique** (visible par tous) qui permet aux autres de verifier que la signature est authentique.

  

Quand Alice envoie 50 credits a Bob, la transaction est signee avec la cle privee d'Alice. N'importe quel appareil du reseau peut verifier, grace a la cle publique d'Alice, que c'est bien elle qui a initie le paiement — et que personne n'a modifie le montant en chemin.

  

Le prototype actuel utilise un **profil de signature Mesh Pay** base sur Monocypher 4.0.2. Il est coherent entre devices MeshPay homogenes, mais il n'est pas annonce comme interoperable Ed25519/RFC8032 avec des stacks externes.

  

---

  

## La monnaie : flexible et configurable

  

MeshPay ne créé pas "une" monnaie : il permet de **définir n'importe quelle monnaie locale** avec ses propres règles. Chaque monnaie est identifiée par un nom, un symbole, et un ensemble de paramètres :




| Paramètre | Exemple | Signification |

| Nom | "Festival Coins" | Identité de la monnaie |

| Symbole | "FC" | Affiche a cote des montants |

| Plafond | 100 000 | Quantite maximale en circulation |

| Frais par transfert | 1 FC | Commission reversee a l'organisateur a chaque paiement |

| Fonte | 5% / mois | d'encourager la circulation de la monnaie plutôt que l'accumulation. |

| Date d'expiration | 31/12/2026 | Apres cette date, plus de transactions |

Seul les deux premier paramètre sont obligatoire, les autres optionnels 

### La fonte (ou demurage)

  

C'est un concept original : les soldes **diminuent légèrement avec le temps**. Par exemple, avec une fonte de 5% par mois, 1 000 crédits deviennent 950 après un mois, puis 902 après deux mois.

  

Pourquoi ? Pour **encourager la circulation de la monnaie** plutôt que la thésaurisation. Dans une monnaie communautaire, il n'y a aucun intérêt a accumuler des crédits — ils sont la pour permettre l'échange. La fonte garantit que la monnaie reste dynamique.

  

### La création de monnaie (minting)

  

Un ou plusieurs appareils désignés comme **maitres** peuvent créer de la monnaie. Ils sont les seuls a pouvoir générer de nouveaux crédits (par exemple, quand un festivalier achète des jetons au comptoir). Cette autorisation est contrôlée par cryptographie : seules les clés publiques des maitres sont reconnues par le reseau.

  

Point important : **plusieurs maitres peuvent coexister**, sans avoir besoin de se coordonner. Chacun peut créer de la monnaie indépendamment, tant que le plafond total n'est pas atteint.

  

---

  

## La sécurité, en détail

  

### Protection contre la double depense

  

Comment empêcher Alice de dépenser deux fois les memes crédits si les appareils ne communiquent pas tous en permanence ?

  

MeshPay utilise un **verrouillage instantané** : des qu'Alice initie un paiement, le montant est immédiatement bloque sur son appareil. Elle ne peut plus l'utiliser, même avant que le destinataire ait confirme. Si la confirmation n'arrive pas dans les 30 secondes, le montant est débloqué et le paiement annule.

  

### Protection anti-rejeu

  

Chaque message signe contient un **nonce** — un nombre aléatoire a usage unique. L'appareil récepteur maintient un cache des derniers nonces vus. Si un attaquant intercepte un message et tente de le rejouer, le nonce sera reconnu comme déjà utilise et le message rejeté.

  

### Code PIN et anti brute-force

  

L'accès aux fonctions sensibles (paiement, création de monnaie) est protégé par un **code PIN a 4 chiffres**. Le PIN n'est jamais stocke en clair : il est transforme par un algorithme couteux en calcul (PBKDF2, 10 000 itérations) qui rend le cassage extrêmement lent.

  

De plus, un système progressif bloque les tentatives :

- 3 échecs : attente de 30 secondes

- 5 échecs : attente de 5 minutes

- 10 échecs : appareil bloque définitivement

  

### Chiffrement du stockage

  

Toutes les données sensibles (clés privées, soldes, PIN) sont **chiffrées directement dans la mémoire flash** de l'appareil grâce au chiffrement AES-256 intégré au hardware de l'ESP32. Meme en dessoudant la puce mémoire, un attaquant ne pourrait pas lire les données.

  

---

  

## Les fonctions de l'administrateur

  

L'appareil maitre dispose de fonctions supplémentaires pour gérer le reseau :

  

- **Créer de la monnaie** : créditer un appareil en nouveaux jetons.

- **Scanner le reseau** : envoyer un ping LoRa et voir tous les appareils a portée (jusqu'a 2 km).

- **Diffuser un message** : envoyer une annonce a tous les appareils ("Fermeture a 18h", "Rechargement au stand B").

- **Renommer un appareil** a distance : utile pour les petits boitiers sans clavier.

- **Configurer le transfert automatique** : les stands de vente peuvent envoyer automatiquement leurs recettes vers un compte central toutes les heures.

  

---

  

## Le materiel

  

MeshPay fonctionne **aujourd'hui** sur deux types de boitiers :

  

### CYD (Cheap Yellow Display)

Le boitier principal : un petit écran tactile de 2.8 pouces, un module LoRa, et une puce ESP32. Compact, peu couteux, et suffisant pour tout faire — payer, recevoir, administrer.

  

### Waveshare ESP32-S3

Un boitier miniature avec un ecran de 1.47 pouces, ideal pour etre porte au poignet ou fixe a un stand. Plus petit, mais avec les memes capacites de paiement.

  

L'interface s'adapte automatiquement a la taille de l'ecran : pave numerique complet sur le grand ecran, navigation simplifiee sur le petit.

  

---

  

## En resume

  

| | MeshPay |

|---|---|

| **Internet requis ?** | Non, jamais |

| **Serveur central ?** | Non, entierement decentralise |

| **Portee** | ~200 m (paiement instantane) / ~2 km (synchronisation) |

| **Vitesse de paiement** | < 1 seconde |

| **Securite** | Signatures Mesh Pay, chiffrement AES-256, PIN protege |

| **Monnaie** | Configurable (nom, plafond, frais, fonte, expiration) |

| **Nombre d'appareils** | Illimite (reseau mesh auto-extensible) |

| **Cout par boitier** | ~15-25 EUR (composants standard) |

  

MeshPay transforme de simples microcontroleurs en un **reseau de paiement autonome, securise et resilient** — prouvant qu'on n'a pas besoin d'internet, de banques ou de serveurs pour echanger de la valeur de maniere fiable.
