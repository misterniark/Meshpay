/**
 * @file debug_console.h
 * @brief Console serie de debug Mesh Pay.
 *
 * Lit des commandes ASCII sur l'UART console (la meme que celle des
 * ESP_LOG*) et execute des callbacks de dump fournis par main.c. Les
 * reponses sont encadrees par des marqueurs textuels pour rester
 * parsables meme quand des logs ESP_LOGI s'intercalent.
 *
 * Commandes supportees, une par ligne (`\n` ou `\r\n`) :
 *   dump_dag        — liste de toutes les transactions presentes en
 *                     memoire (id, type, from, to, amount, seq,
 *                     parents, status, timestamp).
 *   dump_wallet     — own_pubkey, alias, solde disponible, verrous
 *                     actifs, last_melt_timestamp.
 *   dump_currency   — nom, symbole, currency_id, mint_authorities,
 *                     plafond, frais, parametres de fonte.
 *   dump_time       — mode (LAMPORT/MASTER), lamport_counter, etat
 *                     maitre, master_offset_ms.
 *   dump_all        — execute les quatre dumps ci-dessus dans
 *                     l'ordre.
 *   help            — liste les commandes.
 *
 * Format de sortie (chaque ligne JSON sur une ligne separee) :
 *   <<<MESHPAY_DEBUG dump_dag BEGIN seq=42>>>
 *   {"count":3,"max":250}
 *   {"i":0,"id":"abcd...","type":"TRANSFER",...}
 *   ...
 *   <<<MESHPAY_DEBUG dump_dag END>>>
 *
 * En cas d'erreur :
 *   <<<MESHPAY_DEBUG dump_dag ERR seq=42>>>
 *   {"err":"mutex_timeout"}
 *   <<<MESHPAY_DEBUG dump_dag END>>>
 *
 * Le `seq=` est un compteur monotone incremente a chaque commande,
 * utile pour correler input et output cote moniteur.
 *
 * Strategie de gating :
 *   - Quand CONFIG_MESHPAY_DEBUG_CONSOLE=y, l'API ci-dessous est
 *     implementee dans src/debug_console.c.
 *   - Quand =n, l'API se replie sur des stubs static inline qui
 *     retournent ESP_OK sans rien faire — le code appelant n'a pas
 *     besoin d'etre conditionne. Aucune surface d'attaque, zero
 *     octet flash/RAM.
 */

#ifndef DEBUG_CONSOLE_H
#define DEBUG_CONSOLE_H

#include "sdkconfig.h"
#include "esp_err.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================
 * Writer de sortie (utilise par les callbacks de dump)
 * ================================================================ */

/**
 * @brief Ecrivain de ligne pour un dump.
 *
 * Le callback de dump fourni par main.c utilise cette fonction pour
 * emettre chaque ligne JSON sans avoir a connaitre le canal de
 * sortie (UART, fichier de test, etc.). Une seule ligne par appel,
 * SANS le `\n` final — le writer s'en charge.
 *
 * Le writer doit etre appele sous le mutex applicatif si necessaire
 * (le callback de dump est responsable du verrouillage avant
 * d'iterer sur le DAG/wallet).
 *
 * @param line Ligne ASCII a emettre (null-terminated)
 * @param ctx  Contexte opaque fourni au callback
 */
typedef void (*debug_console_writer_fn)(const char *line, void *ctx);

/* ================================================================
 * Callbacks de dump (fournis par main.c)
 * ================================================================ */

/**
 * @brief Signature d'un callback de dump.
 *
 * Chaque callback est responsable de :
 *   1. Acquerir le mutex applicatif (avec timeout) ;
 *   2. Iterer sur l'etat partage (DAG, wallet, currency, time) ;
 *   3. Emettre une ligne JSON par item via `writer(line, ctx)` ;
 *   4. Liberer le mutex.
 *
 * Si l'acquisition du mutex echoue, le callback emet une ligne
 * `{"err":"mutex_timeout"}` et retourne sans rien dumper.
 *
 * @param writer Fonction d'emission de ligne fournie par debug_console
 * @param ctx    Contexte opaque a passer au writer
 */
typedef void (*debug_console_dump_fn)(debug_console_writer_fn writer, void *ctx);

/**
 * @brief Table des callbacks de dump.
 *
 * Tous les champs sont optionnels (NULL = commande non supportee,
 * repondue par une trame ERR `{"err":"not_implemented"}`).
 */
typedef struct {
    debug_console_dump_fn dump_dag;       /**< Implementation de `dump_dag` */
    debug_console_dump_fn dump_wallet;    /**< Implementation de `dump_wallet` */
    debug_console_dump_fn dump_currency;  /**< Implementation de `dump_currency` */
    debug_console_dump_fn dump_time;      /**< Implementation de `dump_time` */
} debug_console_callbacks_t;

/* ================================================================
 * API publique
 * ================================================================ */

#if CONFIG_MESHPAY_DEBUG_CONSOLE

/**
 * @brief Initialise et demarre la console de debug.
 *
 * Cree une tache FreeRTOS de priorite basse qui boucle sur la
 * lecture de l'UART console (UART0 par defaut). Les commandes
 * reconnues declenchent l'appel du callback correspondant.
 *
 * A appeler une seule fois, typiquement en fin de app_main, apres
 * que tous les composants applicatifs sont initialises.
 *
 * @param cbs Callbacks de dump (peut etre NULL pour desactiver
 *            tous les dumps mais garder la commande `help`).
 * @return ESP_OK en cas de succes, ESP_ERR_INVALID_STATE si deja
 *         initialisee, ESP_ERR_NO_MEM si echec de creation tache.
 */
esp_err_t debug_console_init(const debug_console_callbacks_t *cbs);

/* ================================================================
 * API exposee pour les tests unitaires
 * ================================================================
 *
 * Ces declarations existent uniquement pour que les tests Unity
 * puissent appeler le parseur de commandes et le serializer hex
 * sans demarrer la tache UART. Elles ne font PAS partie du contrat
 * public — ne pas les appeler depuis main.c.
 */

/** Identifiants internes des commandes reconnues. */
typedef enum {
    DEBUG_CMD_UNKNOWN = 0,  /**< Commande non reconnue */
    DEBUG_CMD_HELP,
    DEBUG_CMD_DUMP_DAG,
    DEBUG_CMD_DUMP_WALLET,
    DEBUG_CMD_DUMP_CURRENCY,
    DEBUG_CMD_DUMP_TIME,
    DEBUG_CMD_DUMP_ALL,
    DEBUG_CMD_EMPTY,        /**< Ligne vide (pas une erreur) */
} debug_cmd_t;

/**
 * @brief Parse une ligne de commande en token.
 *
 * Ignore espaces de tete et de queue, ignore CR/LF, casse
 * insensible. Une ligne vide retourne DEBUG_CMD_EMPTY (pas
 * DEBUG_CMD_UNKNOWN — un moniteur Python qui envoie des keepalives
 * vides ne doit pas polluer le log d'erreurs).
 *
 * @param line Chaine null-terminated (modifiable, on peut faire un
 *             trim destructif si besoin — la fonction copie ce qu'il
 *             faut en interne et ne mute pas le buffer).
 * @return DEBUG_CMD_* selon la commande reconnue
 */
debug_cmd_t debug_console_parse(const char *line);

/**
 * @brief Serialise un buffer binaire en chaine hex lowercase.
 *
 * Utilise pour dumper les hashes (32 octets) et les cles publiques
 * (32 octets) dans le JSON.
 *
 * @param src      Buffer binaire d'entree
 * @param src_len  Longueur du buffer (octets)
 * @param dst      Buffer de sortie (au moins src_len*2+1 caracteres)
 * @param dst_size Taille du buffer de sortie
 * @return Nombre de caracteres ecrits hors '\0', ou -1 si dst_size
 *         insuffisant.
 */
int debug_console_hex_encode(const uint8_t *src, size_t src_len,
                             char *dst, size_t dst_size);

#else  /* CONFIG_MESHPAY_DEBUG_CONSOLE non defini ou =n */

/* Stubs inline : permettent au code appelant de garder le meme
 * appel `debug_console_init(&cbs)` sans `#if`. Quand le flag est a
 * 'n', l'appel se replie sur un no-op qui retourne ESP_OK. Le code
 * appelant peut tester ESP_OK pour logger un message — mais la
 * tache n'est pas creee et aucune commande UART n'est lue. */

static inline esp_err_t debug_console_init(const debug_console_callbacks_t *cbs)
{
    (void)cbs;
    return ESP_OK;
}

#endif /* CONFIG_MESHPAY_DEBUG_CONSOLE */

#ifdef __cplusplus
}
#endif

#endif /* DEBUG_CONSOLE_H */
