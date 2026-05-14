/**
 * @file power_manager.c
 * @brief Implementation reelle de la machine d'energie (ESP32-S3).
 *
 * Compile uniquement sur ESP32-S3 (cf. main/CMakeLists.txt). Sur le CYD
 * c'est power_manager_stub.c qui est lie.
 *
 * Dependance-pure : aucun include de app_state.h ni de header IDF. Tout
 * passe par le power_manager_config_t injecte.
 */

#include "power_manager.h"

/* Etat interne du module (singleton). */
static power_manager_config_t s_cfg;
static power_state_t          s_state;
static uint32_t               s_last_activity_ms;  /* tronque de get_time_ms */
static uint32_t               s_timeout_ms;

/**
 * Applique une transition d'etat. DOIT etre appele sous lock().
 * Idempotent : si l'etat ne change pas, ne fait rien.
 */
static void enter_state_locked(power_state_t st)
{
    if (st == s_state) {
        return;
    }
    s_state = st;
    if (st == POWER_STATE_ECO) {
        s_cfg.set_backlight(0);
        s_cfg.apply_pm_config(POWER_STATE_ECO);
    } else {
        s_cfg.set_backlight(100);
        s_cfg.apply_pm_config(POWER_STATE_ACTIF);
    }
}

void power_manager_init(const power_manager_config_t *cfg)
{
    s_cfg        = *cfg;
    s_timeout_ms = (cfg->eco_timeout_ms != 0u) ? cfg->eco_timeout_ms
                                               : POWER_ECO_TIMEOUT_MS;
    s_state            = POWER_STATE_ACTIF;
    s_last_activity_ms = (uint32_t)s_cfg.get_time_ms();
    /* Pas de set_backlight/apply_pm_config ici : app_main a deja applique
     * la config de boot (ACTIF). */
}

void power_manager_notify_activity(void)
{
    s_cfg.lock();
    s_last_activity_ms = (uint32_t)s_cfg.get_time_ms();
    enter_state_locked(POWER_STATE_ACTIF);
    s_cfg.unlock();
}

void power_manager_tick(void)
{
    hal_power_source_t src = s_cfg.get_power_source();

    s_cfg.lock();
    if (src == POWER_SOURCE_USB) {
        /* Sur USB : jamais d'ECO. Remonter si on y etait (source changee
         * a chaud, ex. branchement USB pendant l'ECO). */
        enter_state_locked(POWER_STATE_ACTIF);
    } else if (s_state == POWER_STATE_ACTIF) {
        /* BATTERY ou UNKNOWN : ECO apres timeout d'inactivite.
         * Soustraction wraparound-safe sur uint32_t. */
        uint32_t now  = (uint32_t)s_cfg.get_time_ms();
        uint32_t idle = now - s_last_activity_ms;
        if (idle >= s_timeout_ms) {
            enter_state_locked(POWER_STATE_ECO);
        }
    }
    s_cfg.unlock();
}

power_state_t power_manager_get_state(void)
{
    return s_state;
}
