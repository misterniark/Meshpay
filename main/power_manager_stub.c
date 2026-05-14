/**
 * @file power_manager_stub.c
 * @brief Implementation no-op de power_manager pour le CYD (ESP32).
 *
 * Le CYD est le noeud maitre, typiquement sur USB, qui doit rester
 * pleinement reactif. Il n'a pas de gestion d'energie : toutes les
 * fonctions sont des no-ops et get_state renvoie toujours ACTIF.
 *
 * Compile uniquement sur ESP32 (cf. main/CMakeLists.txt). Sur le S3
 * c'est power_manager.c (impl reelle) qui est lie.
 */

#include "power_manager.h"

void power_manager_init(const power_manager_config_t *cfg)
{
    (void)cfg;
}

void power_manager_notify_activity(void)
{
}

void power_manager_tick(void)
{
}

power_state_t power_manager_get_state(void)
{
    return POWER_STATE_ACTIF;
}
