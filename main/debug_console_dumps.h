/**
 * @file debug_console_dumps.h
 * @brief Enregistrement des callbacks de dump pour debug_console.
 *
 * Une seule fonction publique : `debug_console_register_dumps()`. Le
 * fichier .c correspondant est selectionne par CMake :
 *
 *   - `debug_console_dumps.c`      : impl reelle (compile si DEBUG_CONSOLE=y)
 *   - `debug_console_dumps_stub.c` : no-op (compile sinon)
 *
 * Le code applicatif n'a plus aucun `#if CONFIG_MESHPAY_DEBUG_CONSOLE`
 * autour de l'enregistrement ni des definitions de callbacks.
 */

#ifndef MESHPAY_DEBUG_CONSOLE_DUMPS_H
#define MESHPAY_DEBUG_CONSOLE_DUMPS_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Enregistre les 4 callbacks de dump (DAG, wallet, currency, time)
 *        aupres du composant debug_console.
 *
 * Sur stub : no-op. Sur impl reelle : appelle `debug_console_init()`.
 */
void debug_console_register_dumps(void);

#ifdef __cplusplus
}
#endif

#endif /* MESHPAY_DEBUG_CONSOLE_DUMPS_H */
