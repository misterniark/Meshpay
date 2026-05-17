# Ledger flash journal — decision 2026-05-17

## Objectif

Reduire l'usure flash du ledger sans changer l'API applicative.

Avant cette phase, chaque sauvegarde de fenetre TX ou attestation reecrivait un
snapshot dual-slot complet. C'etait robuste aux coupures, mais trop couteux si
la frequence d'ecriture augmente.

Le backend `ledger_store` utilise maintenant:

- des snapshots dual-slot pour checkpoint, fenetre TX et fenetre attestations;
- un journal append-only compact dans la fin de la partition `storage`;
- une compaction automatique quand le journal est plein.

## Layout storage

Partition `storage`: offset logique `0x00000`, taille `0x5f000`.

- `0x00000..0x07fff`: checkpoint dual-slot;
- `0x08000..0x27fff`: TX window dual-slot;
- `0x28000..0x37fff`: attestation window dual-slot;
- `0x38000..0x5efff`: journal append-only.

## Format record journal

Chaque record:

- magic `MPJL`;
- version;
- generation monotone;
- type:
  - `1`: transaction;
  - `2`: attestation;
- payload length;
- checksum payload;
- checksum header;
- payload;
- padding `0xff` jusqu'a alignement 16 octets.

L'alignement 16 octets garde les writes compatibles avec la partition chiffrée.

## Recovery

Au boot ou a la lecture:

1. charger le meilleur snapshot dual-slot valide;
2. scanner le journal depuis son debut;
3. verifier magic/version/type/CRC header/payload;
4. appliquer chaque record valide avec dedup:
   - TX: append-or-replace par `tx_id`;
   - attestation: unique par `(attester_key, tx_id)`;
5. s'arreter au premier record absent, tronque ou invalide.

Un record coupe par une coupure d'alimentation est donc ignore, et tous les
records precedents restent utilisables.

## Compaction

Quand le journal n'a plus assez d'espace:

1. materialiser `snapshot + journal`;
2. reecrire les snapshots dual-slot TX/attestations;
3. effacer le journal;
4. reprendre l'append du record courant.

Le checkpoint garde son chemin dual-slot dedie, car il represente l'etat
comptable consolide.

## Limites restantes

- Pas encore de test host/mocked-partition qui simule tous les types de coupure
  entre header, payload et padding.
- La compaction reecrit encore des snapshots complets, mais beaucoup moins
  souvent que l'ancien modele.
- Le journal est local au device; il ne remplace pas la preuve historique pour
  un device absent plus longtemps que la fenetre durable.
