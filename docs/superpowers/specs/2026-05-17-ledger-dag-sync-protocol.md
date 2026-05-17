# Mesh Pay - Ledger DAG Sync Protocol

Date: 2026-05-17

Objectif: separer clairement l'etat comptable, le journal recent et la
synchronisation radio pour faire converger les devices apres reboot, perte
radio ou coupure d'alimentation.

## Modeles

### Checkpoint

Le checkpoint est l'etat comptable consolide.

Il contient:

- les soldes par compte;
- le dernier timestamp consolide;
- le dernier tx_id consolide;
- les informations globales necessaires au calcul de solde.

Invariant:

- une TX couverte par le checkpoint ne doit pas etre recomptee depuis la DAG RAM;
- un parent couvert par le checkpoint est un parent valide a la frontiere;
- le checkpoint doit survivre a une coupure pendant ecriture.

### Fenetre TX durable

La fenetre TX durable est un journal recent verifiable.

Elle contient les TX recentes utiles pour:

- reconstruire la DAG RAM apres reboot;
- afficher l'historique utilisateur;
- repropager les TX a un peer en retard;
- verifier les parents proches de la frontiere checkpoint.

Invariant:

- chaque TX exposee par la fenetre doit passer structure + signature/hash;
- la fenetre peut contenir des TX deja checkpointed, mais elles ne doivent pas
  etre reinjectees dans la DAG RAM si `tx.timestamp <= checkpoint.timestamp`;
- la fenetre doit etre idempotente: reinserer la meme TX ne change pas l'etat;
- la fenetre doit survivre a une coupure pendant ecriture.

### Attestation durable

Une attestation prouve que le destinataire d'une TX l'a confirmee.

Invariant:

- une attestation n'est applicable que si elle est signee par `tx.to`;
- une attestation recue avant la TX doit etre conservee et rejouee plus tard;
- rejouer une attestation deja appliquee doit etre sans effet.

## Cycle de vie d'une transaction

Le module `tx_lifecycle` est le point d'autorite pour les transitions
applicatives de statut. Les handlers, le replay d'attestations et les chemins
de rollback doivent passer par lui plutot que modifier directement le DAG.

1. Creation locale: la TX est creee avec parents connus et `seq` monotone.
2. Emission: la TX part via ESP-NOW et/ou LoRa.
3. Reception TRANSFER: le status reseau est force a `LOCKED`.
4. Confirmation: le destinataire confirme par ACK/attestation.
5. Commit: le checkpoint consolide les TX `CONFIRMED`.
6. Prune: la DAG RAM retire les TX confirmees couvertes par checkpoint.
7. Retention: la fenetre TX conserve les TX recentes pour historique et sync.

Invariant:

- `CONFIRMED` et `CANCELLED` sont terminaux;
- `LOCKED -> CONFIRMED` et `LOCKED -> CANCELLED` sont les seules transitions
  applicatives normales;
- le status d'une TX recue du reseau n'est jamais une preuve de confirmation.
- un ACK n'est valide que si `ack_sender == tx.to`;
- une attestation n'est valide que si sa signature couvre `tx_id` et que
  `attester_key == tx.to`;
- une attestation recue avant la TX doit rester rejouable sans changer le
  resultat si elle est recue plusieurs fois;
- une attestation valide ne peut jamais ressusciter une TX `CANCELLED`.

## Synchronisation LoRa

Chaque cycle envoie:

1. `DAG_SUMMARY` signe: checkpoint timestamp, dernier timestamp TX, count fenetre,
   tips recentes.
2. Si le peer annonce un etat plus recent: `DAG_REQUEST` signe.
3. Le peer repond avec des TX recentes et des attestations recentes.

Invariant:

- tous les messages de controle DAG sont signes par l'emetteur;
- une request ciblee doit etre ignoree par les autres peers;
- une reponse doit etre bornee et paginable;
- recevoir les memes TX/attestations plusieurs fois doit etre sans effet.

## Persistance et coupure d'alimentation

Les blobs durables utilisent deux slots par region, avec generation monotone.

Ecriture:

1. construire un blob complet en RAM;
2. choisir le slot alterne depuis la generation suivante;
3. effacer uniquement ce slot;
4. ecrire le blob complet;
5. au boot, charger le slot valide de plus grande generation.

Invariant:

- si une coupure arrive pendant erase/write du nouveau slot, l'ancien slot reste
  chargeable;
- si les deux slots sont invalides, le composant doit revenir en mode
  "pas de checkpoint/fenetre" sans planter;
- un checksum couvre tout le payload utile.

## Etats utilisateur

L'UI doit distinguer:

- solde local convergé;
- paiement `LOCKED` en attente;
- sync/rattrapage en cours;
- historique charge depuis la fenetre durable.

Invariant:

- l'utilisateur ne doit pas voir un solde "definitif" si des TX locales restent
  `LOCKED` ou si le device vient de redemarrer sans avoir encore recu de summary.

## Prochaines phases

Phase E1: double-slot durable pour checkpoint, fenetre TX et attestations.

Phase E2: tests de coupure simulee sur les blobs ledger.

Phase E3: pagination de `DAG_REQUEST` avec curseur ou plages temporelles.

Phase E4: tests de convergence multi-device avec pertes, doublons, ordre aleatoire
et reboot au milieu d'une transaction. Un premier socle existe dans
`test_tx_lifecycle_chaos.c` pour les transitions locales; il reste a ajouter
des tests bout-en-bout lora_sync + ledger durable.

Phase E5: decision crypto definitive: soit Monocypher 4.0.2 comme format ferme,
soit migration vers une signature Ed25519 RFC8032 validee par vecteurs officiels.
