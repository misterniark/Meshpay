/**
 * @file ui_state.h
 * @brief Contexte UI — pointeurs vers l'etat global en lecture seule.
 *
 * La ui_task accede a l'etat du systeme (wallet, DAG, peers, etc.)
 * via ces pointeurs. Toute lecture doit se faire sous le mutex
 * s_state_mutex. Les actions (payer, MINT, etc.) passent par la
 * queue de commandes UI → core_task.
 */

#ifndef UI_STATE_H
#define UI_STATE_H

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"

#include "hal/hal_display.h"
#include "hal/hal_storage.h"
#include "crypto/crypto_types.h"
#include "wallet/wallet.h"
#include "dag/dag.h"
#include "currency/currency_config.h"
#include "comm/comm_msg.h"
#include "comm/comm_event.h"

/**
 * Resultat d'un ping : cle publique + alias du device repondant.
 * Replique de la struct dans main.c pour que l'UI puisse y acceder.
 */
typedef struct {
    public_key_t key;
    char         alias[COMM_MSG_ALIAS_MAX + 1];
    uint8_t      alias_len;
} ui_ping_result_t;

/**
 * Resultat de paiement notifie par core_task vers l'UI.
 * Ecrit par le core, lu et reset par la ui_task.
 */
typedef enum {
    UI_PAY_FEEDBACK_NONE = 0, /* Pas de feedback en attente */
    UI_PAY_FEEDBACK_OK,       /* Paiement insere dans le DAG */
    UI_PAY_FEEDBACK_FAIL,     /* Erreur lors du paiement */
    UI_PAY_FEEDBACK_NO_FUNDS, /* Solde insuffisant */
} ui_pay_feedback_t;

/**
 * Commandes envoyees par l'UI vers core_task.
 *
 * L'UI ne touche jamais directement au DAG ou au wallet.
 * Elle poste une commande dans la queue, et core_task l'execute
 * sous le mutex.
 */
typedef enum {
    UI_CMD_PAY,            /* Initier un paiement */
    UI_CMD_MINT,           /* Creer des credits (maitre) */
    UI_CMD_DISCOVER_PEERS, /* Lancer un DISCOVER ESP-NOW */
    UI_CMD_BROADCAST_TEXT, /* Envoyer un broadcast texte (maitre) */
    UI_CMD_PING,           /* Lancer un ping LoRa (maitre) */
    UI_CMD_SET_ALIAS,      /* Renommer un device distant (maitre) */
    UI_CMD_SET_BENEFICIARY,/* Configurer auto-forward (maitre) */
} ui_cmd_type_t;

typedef struct {
    ui_cmd_type_t type;

    union {
        /* UI_CMD_PAY */
        struct {
            public_key_t to;
            uint32_t     amount;
        } pay;

        /* UI_CMD_MINT */
        struct {
            public_key_t to;
            uint32_t     amount;
        } mint;

        /* UI_CMD_BROADCAST_TEXT */
        struct {
            char    text[COMM_MSG_BROADCAST_TEXT_MAX + 1];
            uint8_t text_len;
        } broadcast;

        /* UI_CMD_SET_ALIAS */
        struct {
            public_key_t target;
            char         alias[COMM_MSG_ALIAS_MAX + 1];
            uint8_t      alias_len;
        } set_alias;

        /* UI_CMD_SET_BENEFICIARY */
        struct {
            public_key_t target;
            public_key_t beneficiary;
            uint16_t     interval_min;
        } set_beneficiary;
    } data;
} ui_cmd_t;

/**
 * Contexte UI — passe a chaque ecran via ui_task.
 *
 * Contient les pointeurs vers l'etat global (lecture sous mutex)
 * et la queue de commandes (ecriture sans mutex).
 */
typedef struct ui_ctx_s {
    /* Acces au hardware */
    hal_display_t     *display;
    hal_storage_t     *storage;

    /* Mutex pour acceder aux donnees partagees */
    SemaphoreHandle_t  state_mutex;

    /* Queue pour envoyer des commandes a core_task */
    QueueHandle_t      cmd_queue;

    /* Etat global en lecture seule (protege par state_mutex) */
    wallet_t           *wallet;
    dag_t              *dag;
    currency_config_t  *currency;
    const public_key_t *own_pubkey;  /* Cle publique propre (sans cle privee) */
    bool                is_master;   /* true si le device est un maitre (mint_authority) */
    char               *device_alias;
    uint8_t            *device_alias_len;

    /* Broadcast en attente */
    bool                     *broadcast_pending;
    comm_msg_broadcast_t     *pending_broadcast;

    /* Peers decouverts */
    comm_peer_info_t  *peers;
    uint32_t          *peer_count;

    /* Resultats de ping */
    ui_ping_result_t  *ping_results;
    uint32_t          *ping_result_count;

    /* Beneficiaire auto-forward */
    public_key_t      *beneficiary_key;
    uint16_t          *forward_interval_min;

    /* Feedback paiement (core → UI) */
    volatile ui_pay_feedback_t *pay_feedback;

    /* Resolution ecran (cache) */
    uint16_t           screen_w;
    uint16_t           screen_h;
    bool               is_small_screen; /* true si Waveshare (w < 200) */

    /** Fonction de calcul du solde apres fonte (lecture seule, pas de mutation).
     *  NULL si la fonte est desactivee. */
    uint32_t (*compute_melted_balance)(uint32_t raw_balance);

    /** Fonction de calcul du solde du owner (checkpoint + DAG).
     *  Evite le double-comptage initial_balance (bug C3) et reste coherent
     *  apres pruning du DAG (bug I1). NULL interdit. */
    uint32_t (*get_owner_balance)(void);

    /** Signale une interaction utilisateur au gestionnaire d'energie.
     *  Appele par ui_task sur chaque touch detecte. NULL autorise
     *  (le gestionnaire d'energie est optionnel selon la cible). */
    void (*notify_activity)(void);
} ui_ctx_t;

#endif /* UI_STATE_H */
