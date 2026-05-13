/**
 * @file ui_dispatch.h
 * @brief Dispatch des commandes UI vers les ops applicatives.
 *
 * core_task draine periodiquement s_ui_cmd_queue et appelle
 * handle_ui_command (sous s_state_mutex) pour chaque commande.
 */

#ifndef MESHPAY_UI_DISPATCH_H
#define MESHPAY_UI_DISPATCH_H

#include "ui/ui_state.h"

#ifdef __cplusplus
extern "C" {
#endif

void handle_ui_command(const ui_cmd_t *cmd);

#ifdef __cplusplus
}
#endif

#endif /* MESHPAY_UI_DISPATCH_H */
