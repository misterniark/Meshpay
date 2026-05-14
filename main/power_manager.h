/**
 * @file power_manager.h
 * @brief Machine d'etats de gestion de l'energie (ACTIF / ECO).
 *
 * Sur le Waveshare ESP32-S3 (impl reelle power_manager.c), gere la
 * transition entre un mode ACTIF (comportement normal) et un mode ECO
 * (backlight eteint + CPU frequency scaling), declenchee par inactivite.
 * Sur le CYD (power_manager_stub.c), tout est no-op.
 *
 * CONTRAINTE D'ISOLATION : power_manager.c n'inclut JAMAIS app_state.h
 * ni aucun header interne de main/. Toutes ses dependances passent par
 * le power_manager_config_t injecte a l'init. Il ne depend que de son
 * propre header + hal/hal_power.h + <stdint.h>/<stdbool.h>. Cela le rend
 * compilable en isolation pour ses tests natifs.
 */

#ifndef MESHPAY_POWER_MANAGER_H
#define MESHPAY_POWER_MANAGER_H

#include <stdint.h>
#include "hal/hal_power.h"

/** Timeout d'inactivite avant passage en ECO (ms), sur batterie. */
#define POWER_ECO_TIMEOUT_MS 120000u

/**
 * Etats de la machine d'energie.
 */
typedef enum {
    POWER_STATE_ACTIF,  /* Comportement normal : backlight 100, freq max */
    POWER_STATE_ECO,    /* Eco : backlight 0, CPU frequency scaling actif */
} power_state_t;

/**
 * Configuration injectee a l'init.
 *
 * Tous les champs sont des callbacks/valeurs fournis par l'appelant —
 * c'est ce qui rend power_manager.c testable en isolation.
 */
typedef struct {
    /** Source de temps monotone en ms (firmware : get_time_ms_wrapper). */
    uint64_t (*get_time_ms)(void);

    /** Lit la source d'alimentation courante (firmware : hal_power). */
    hal_power_source_t (*get_power_source)(void);

    /** Regle le backlight : 0 = eteint, 100 = max (firmware : hal_display). */
    void (*set_backlight)(uint8_t pct);

    /** Applique la config esp_pm correspondant a l'etat (firmware : esp_pm). */
    void (*apply_pm_config)(power_state_t state);

    /** Verrou applicatif : protege l'etat partage entre core_task et ui_task.
     *  Firmware : mutex FreeRTOS dedie. Test (mono-thread) : no-op. */
    void (*lock)(void);
    void (*unlock)(void);

    /** Timeout d'inactivite en ms. 0 => POWER_ECO_TIMEOUT_MS. */
    uint32_t eco_timeout_ms;
} power_manager_config_t;

/**
 * @brief Initialise la machine d'energie. Etat initial : ACTIF.
 *
 * Ne touche pas au backlight ni a esp_pm : app_main a deja applique la
 * config de boot (ACTIF). Copie cfg en interne.
 */
void power_manager_init(const power_manager_config_t *cfg);

/**
 * @brief Signale une interaction (touch, evenement reseau, commande UI).
 *
 * Met a jour l'horodatage de derniere activite et, si on etait en ECO,
 * repasse immediatement en ACTIF. Thread-safe (utilise lock/unlock).
 */
void power_manager_notify_activity(void);

/**
 * @brief A appeler periodiquement (firmware : chaque tour de core_task).
 *
 * Relit la source d'alimentation et decide la transition ACTIF -> ECO
 * si le device est sur batterie et inactif depuis >= eco_timeout_ms.
 * Sur USB : force le retour en ACTIF. Thread-safe.
 */
void power_manager_tick(void);

/**
 * @brief Retourne l'etat courant.
 *
 * Lecture best-effort non protegee par le verrou : s_state est volatile
 * et les ecritures sont sous lock, mais l'appelant peut obtenir une
 * valeur d'une iteration de retard. Acceptable comme hint d'etat, pas
 * pour une decision critique.
 */
power_state_t power_manager_get_state(void);

#endif /* MESHPAY_POWER_MANAGER_H */
