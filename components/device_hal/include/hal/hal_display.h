/**
 * @file hal_display.h
 * @brief Interface abstraite pour l'affichage et le tactile (HAL).
 *
 * Définit une vtable de 5 opérations pour piloter un écran avec
 * entrée tactile. Les signatures sont conçues pour s'intégrer
 * facilement avec LVGL :
 * - flush() correspond au lv_disp_drv_t.flush_cb
 * - touch_read() correspond au lv_indev_drv_t.read_cb
 *
 * L'implémentation concrète gère le bus SPI/I2C, le contrôleur
 * d'affichage (ILI9341, ST7789, etc.) et le contrôleur tactile
 * (XPT2046, FT6336, etc.).
 *
 * Portabilité : ce header n'inclut aucun header spécifique plateforme.
 */

#ifndef HAL_DISPLAY_H
#define HAL_DISPLAY_H

#include "hal/hal_types.h"

/**
 * Point tactile : position et état de pression.
 */
typedef struct {
    uint16_t x;       /* Coordonnée X en pixels */
    uint16_t y;       /* Coordonnée Y en pixels */
    bool     pressed; /* true si l'écran est touché */
} hal_touch_point_t;

/**
 * Vtable d'affichage et entrée tactile.
 *
 * Chaque pointeur de fonction reçoit le contexte opaque `ctx`.
 */
typedef struct {
    /**
     * Initialiser l'écran et le contrôleur tactile.
     * Configure les bus SPI/I2C, le rétroéclairage, etc.
     * Doit être appelé une seule fois au démarrage.
     *
     * @param ctx Contexte opaque
     * @return HAL_OK en cas de succès
     */
    hal_err_t (*init)(void *ctx);

    /**
     * Envoyer une zone de pixels à l'écran.
     *
     * Les pixels sont au format RGB565 (16 bits par pixel).
     * La zone est définie par le rectangle (x1,y1)-(x2,y2) inclus.
     *
     * @param x1    Coordonnée X du coin supérieur gauche
     * @param y1    Coordonnée Y du coin supérieur gauche
     * @param x2    Coordonnée X du coin inférieur droit
     * @param y2    Coordonnée Y du coin inférieur droit
     * @param color Tableau de pixels RGB565, taille = (x2-x1+1)*(y2-y1+1)
     * @param ctx   Contexte opaque
     * @return HAL_OK en cas de succès
     */
    hal_err_t (*flush)(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2,
                       const uint16_t *color, void *ctx);

    /**
     * Lire l'état actuel du tactile.
     *
     * @param point [out] Position et état de pression
     * @param ctx   Contexte opaque
     * @return HAL_OK en cas de succès
     */
    hal_err_t (*touch_read)(hal_touch_point_t *point, void *ctx);

    /**
     * Régler la luminosité du rétroéclairage.
     *
     * @param brightness Luminosité de 0 (éteint) à 100 (max)
     * @param ctx        Contexte opaque
     * @return HAL_OK en cas de succès
     */
    hal_err_t (*set_backlight)(uint8_t brightness, void *ctx);

    /**
     * Obtenir la résolution de l'écran en pixels.
     *
     * @param width  [out] Largeur en pixels
     * @param height [out] Hauteur en pixels
     * @param ctx    Contexte opaque
     * @return HAL_OK en cas de succès
     */
    hal_err_t (*get_resolution)(uint16_t *width, uint16_t *height, void *ctx);

    /**
     * [F-HW-010] Mettre l'écran en veille pour économiser la batterie.
     *
     * Coupe le rétroéclairage (équivalent `set_backlight(0)`) ET envoie
     * la commande SLPIN au contrôleur LCD pour minimiser la
     * consommation. Sans cet appel, le contrôleur reste actif et
     * consomme inutilement.
     *
     * ⚠️ Périmètre limité — cet appel ne couvre QUE le sous-système
     * écran. Il ne touche pas au tactile (qui reste sur son bus SPI/I2C)
     * et ne configure aucun wake-source. En particulier :
     *
     * - **Light sleep système** : le tactile continue de fonctionner
     *   naturellement (CPU + périphs alimentés), une IRQ tactile
     *   réveillera l'ESP32 sans configuration supplémentaire.
     * - **Deep sleep système** : seul le RTC reste actif. Pour
     *   réveiller le device sur un toucher, la couche `power_manager`
     *   doit configurer la pin IRQ tactile comme RTC GPIO via
     *   `esp_sleep_enable_ext0_wakeup(touch_irq_pin, 0)`. Ce HAL ne le
     *   fait PAS — c'est volontaire pour ne pas coupler `hal_display`
     *   à la stratégie de power management.
     *
     * Au wake (depuis quelque source que ce soit), l'appelant doit
     * appeler `init()` à nouveau ou un futur `wake()` pour envoyer
     * SLPOUT et restaurer le backlight.
     *
     * Peut être NULL pour les drivers qui ne supportent pas la veille
     * (mock, futurs drivers). L'appelant doit vérifier avant d'invoquer.
     *
     * @param ctx Contexte opaque
     * @return HAL_OK en cas de succès
     */
    hal_err_t (*sleep)(void *ctx);

    /** Contexte opaque de l'implémentation */
    void *ctx;
} hal_display_t;

#endif /* HAL_DISPLAY_H */
