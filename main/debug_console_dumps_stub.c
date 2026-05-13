/**
 * @file debug_console_dumps_stub.c
 * @brief Stub no-op pour la facade debug_console_dumps.
 *
 * Compile uniquement quand `CONFIG_MESHPAY_DEBUG_CONSOLE=n` (selection
 * CMake). Aucune dependance sur l'etat applicatif, zero RAM, zero flash.
 */

#include "debug_console_dumps.h"

void debug_console_register_dumps(void)
{
    /* no-op : la console de debug est desactivee dans cette config. */
}
