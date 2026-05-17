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

## Resolution des branches concurrentes

Deux TX differentes avec le meme couple `(from, seq)` representent une
equivocation de l'emetteur. Elles peuvent etre toutes les deux valides
cryptographiquement et arriver dans des ordres differents selon les devices.

Politique locale:

1. verifier structure, hash, signature et autorite MINT si necessaire;
2. detecter les TX deja presentes avec le meme `(from, seq)`;
3. choisir comme gagnante canonique la TX dont `tx_id` est le plus petit en
   ordre lexicographique;
4. conserver les branches perdantes en `CANCELLED`;
5. ne comptabiliser que la gagnante non annulee;
6. exclure les branches `CANCELLED` des tips actifs de `DAG_SUMMARY`.

Invariant:

- deux devices qui voient le meme ensemble de branches conflictuelles doivent
  converger vers le meme gagnant, quel que soit l'ordre de reception;
- une branche perdante reste presente pour audit/dedup/replay, mais ne doit pas
  affecter le solde;
- ce mecanisme corrige la convergence de la fenetre DAG recente. Il ne remplace
  pas encore une preuve de checkpoint historique pour un device revenu apres une
  tres longue absence si le conflit a deja ete consolide puis prune ailleurs.

## Synchronisation LoRa

Chaque cycle envoie:

1. `DAG_SUMMARY` signe: checkpoint timestamp, dernier timestamp TX, count fenetre,
   tips recentes.
2. Si le peer annonce un etat plus recent: `DAG_REQUEST` signe.
3. Le peer repond avec une page bornee de TX confirmees, triee par
   `(timestamp, tx_id)`, puis republie un `DAG_SUMMARY` signe comme accusé
   d'etat/reprise.
4. Si le demandeur reste en retard, le summary suivant declenche une nouvelle
   request avec le curseur avance. Si un paquet a ete perdu, la meme plage peut
   etre redemandee sans effet de bord.

Pagination:

- `DAG_REQUEST.since_timestamp` est le curseur de reprise;
- `DAG_REQUEST.max_count` borne la page radio;
- le collecteur doit choisir les plus anciennes TX confirmees avec
  `timestamp > since_timestamp`, pas "les premieres trouvees" dans l'ordre
  interne du DAG;
- a la reception d'un summary plus recent, le demandeur recule le curseur local
  d'1 ms pour tolerer plusieurs TX avec le meme timestamp. Les doublons sont
  attendus et dedupes par `dag_merge_transaction`.
- une meme request `(peer, since)` est throttlee pendant 5 s pour eviter une
  boucle radio immediate si le summary de fin de reponse arrive avant que la
  core task ait integre les TX, ou si la page demandee est temporairement vide.

Invariant:

- tous les messages de controle DAG sont signes par l'emetteur;
- une request ciblee doit etre ignoree par les autres peers;
- une reponse doit etre bornee, paginable et deterministe;
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

Phase E3: pagination de `DAG_REQUEST` avec curseur temporel — realisee pour la
fenetre recente: page deterministe, recouvrement de 1 ms, summary de fin de
reponse comme ACK d'etat.

Phase E4: tests de convergence multi-device avec pertes, doublons, ordre aleatoire
et reboot au milieu d'une transaction. Un premier socle existe dans
`test_tx_lifecycle_chaos.c` pour les transitions locales; il reste a ajouter
des tests bout-en-bout lora_sync + ledger durable. La resolution deterministe
des conflits `(from, seq)` est couverte dans `test_dag.c`.

Phase E5: decision crypto court terme realisee dans
`2026-05-17-crypto-signature-profile.md`: Monocypher 4.0.2 est documente
comme profil ferme Mesh Pay, non annonce RFC8032. Une migration vers une
signature Ed25519 RFC8032 standard reste requise pour un systeme ouvert.
