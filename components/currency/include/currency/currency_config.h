/**
 * @file currency_config.h
 * @brief Configuration de la monnaie locale.
 *
 * Définit la structure de configuration partagée par tous les devices
 * d'un même réseau/événement. La config est chargée au démarrage et
 * ne change pas pendant l'exécution.
 *
 * Le currency_id permet d'isoler les monnaies : deux événements
 * géographiquement proches ne mélangent pas leurs soldes, car
 * chaque transaction porte le currency_id de sa monnaie.
 *
 * Le core/ ne dépend pas de ce module au niveau CMake — l'injection
 * se fait via passage de la config dans main.c.
 */

#ifndef CURRENCY_CONFIG_H
#define CURRENCY_CONFIG_H

#include "crypto/crypto_types.h"
#include <stdint.h>
#include <stdbool.h>

/* ================================================================
 * Constantes
 * ================================================================ */

/** Taille maximale du nom de la monnaie */
#define CURRENCY_NAME_MAX    32

/** Taille maximale du symbole */
#define CURRENCY_SYMBOL_MAX   8

/** Nombre maximum de clés maîtres autorisées pour le MINT */
#define CURRENCY_MAX_MINT_AUTHORITIES 8

/** Taille maximale de la description de l'événement */
#define CURRENCY_DESCRIPTION_MAX 64

/**
 * [F-CU-012] Base des basis points pour le calcul de la fonte.
 *
 * 10000 bps = 100 %. Un tick à `melt_bps = 100` retire 1 % du solde.
 * Constante définie ici pour éviter la duplication de la valeur dans
 * les boucles de calcul de fonte et garantir la cohérence si la
 * granularité venait à changer.
 */
#define CURRENCY_BPS_SCALE 10000U

/* ================================================================
 * Mode de fonte
 * ================================================================ */

/** Mode de calcul du volume de fonte par tick */
typedef enum {
    MELT_MODE_BPS   = 0, /* Pourcentage du solde en basis points (100 = 1%) */
    MELT_MODE_FIXED = 1, /* Montant fixe retiré par tick */
} melt_volume_mode_t;

/* ================================================================
 * Structure de configuration
 * ================================================================ */

/**
 * Configuration complète d'une monnaie.
 *
 * Initialisée une fois au boot, accessible en lecture seule ensuite.
 * Tous les devices d'un même réseau doivent avoir la même config
 * pour que les soldes restent cohérents.
 */
typedef struct {
    /* --- Identité et affichage --- */
    uint32_t currency_id;                     /* Identifiant unique (hash tronqué) */
    char     name[CURRENCY_NAME_MAX + 1];     /* Nom lisible (null-terminated) */
    char     symbol[CURRENCY_SYMBOL_MAX + 1]; /* Symbole compact (null-terminated) */
    char     description[CURRENCY_DESCRIPTION_MAX + 1]; /* Description événement (null-terminated) */
    uint8_t  decimals;                        /* Position de la virgule (ex: 2 → 500 = "5,00") */

    /* --- Politique monétaire --- */
    public_key_t mint_authorities[CURRENCY_MAX_MINT_AUTHORITIES]; /* Clés autorisées pour MINT */
    uint8_t      mint_authority_count;         /* Nombre de clés dans mint_authorities */
    uint64_t     max_supply;                   /* Plafond de création monétaire (0 = illimité) */
    uint64_t     valid_until;                  /* Timestamp d'expiration (0 = pas d'expiration) */
    uint32_t     initial_balance;              /* Solde initial crédité au premier boot (0 = aucun) */

    /* --- Règles de transaction --- */
    uint32_t min_transfer_amount;             /* Montant minimum par TRANSFER */
    uint32_t max_transfer_amount;             /* Montant maximum par TRANSFER (0 = pas de plafond) */
    uint32_t transfer_fee;                    /* Frais par transfert (brûlés, 0 = pas de frais) */
    uint32_t transfer_cooldown_ms;            /* Délai minimum entre 2 TRANSFER émis (ms, 0 = pas de cooldown) */

    /* --- Monnaie fondante --- */
    bool              melt_enabled;           /* Active la fonte périodique */
    uint32_t          melt_period_seconds;    /* Intervalle entre deux ticks de fonte */
    melt_volume_mode_t melt_volume_mode;      /* BPS ou FIXED */
    uint16_t          melt_bps;               /* Pourcentage par tick en basis points (100 = 1%) */
    uint32_t          melt_fixed_amount;      /* Montant fixe retiré par tick */
} currency_config_t;

#endif /* CURRENCY_CONFIG_H */
