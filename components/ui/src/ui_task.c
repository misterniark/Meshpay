/**
 * @file ui_task.c
 * @brief Tache FreeRTOS pour l'interface graphique LVGL.
 *
 * Initialise LVGL, cree le display et l'input device, puis
 * boucle sur lv_timer_handler() pour le rendu.
 *
 * Bridge entre le HAL display et les callbacks LVGL :
 * - flush_cb : HAL flush() -> lv_display_flush_ready()
 * - read_cb  : HAL touch_read() -> lv_indev_data_t
 *
 * Le tick LVGL est fourni par esp_timer (1 appel/ms).
 */

#include "ui/ui_state.h"
#include "ui/ui_manager.h"
#include "ui/ui_theme.h"
#include "ui/ui_screens.h"
#include "ui/ui_pin.h"

#include "lvgl.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "ui_task";

/* ================================================================
 * Callbacks bridge LVGL <-> HAL
 * ================================================================ */

/**
 * Callback flush LVGL : envoie une zone de pixels au display via HAL.
 *
 * Appele par LVGL quand une zone a ete rendue. On delegue au HAL
 * puis on notifie LVGL que le flush est termine.
 */
static void lvgl_flush_cb(lv_display_t *disp, const lv_area_t *area,
                           uint8_t *px_map)
{
    ui_ctx_t *ctx = lv_display_get_user_data(disp);

    /*
     * Swap endian RGB565 (Lot E.5).
     *
     * LVGL produit ses pixels RGB565 en little-endian (octet bas
     * d'abord en memoire). Les panneaux JD9853 / ST7789 attendent
     * les pixels en big-endian sur le bus SPI (octet haut d'abord,
     * conformement aux specs MIPI DCS). Sans ce swap, chaque pixel
     * 16-bit est lu byte-swapped par le controleur LCD : composantes
     * R/G/B melangees -> teinte violette sur le blanc + flou
     * (observe au smoke test 2026-05-12 apres correction du pinout).
     *
     * lv_draw_sw_rgb565_swap() est fournie par LVGL v9 et fait
     * l'inversion in-place sur le buffer. Cout: ~1 cycle/pixel avec
     * SIMD Xtensa, negligeable face au temps de transfert SPI.
     */
    size_t pixel_count = (size_t)(area->x2 - area->x1 + 1)
                       * (size_t)(area->y2 - area->y1 + 1);
    lv_draw_sw_rgb565_swap(px_map, pixel_count);

    ctx->display->flush(
        (uint16_t)area->x1, (uint16_t)area->y1,
        (uint16_t)area->x2, (uint16_t)area->y2,
        (const uint16_t *)px_map,
        ctx->display->ctx
    );

    lv_display_flush_ready(disp);
}

/**
 * Callback tactile LVGL : lit l'etat du tactile via HAL.
 *
 * Appele periodiquement par LVGL pour sonder l'input.
 */
static void lvgl_touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    ui_ctx_t *ctx = lv_indev_get_user_data(indev);

    hal_touch_point_t pt;
    hal_err_t err = ctx->display->touch_read(&pt, ctx->display->ctx);

    if (err == HAL_OK && pt.pressed) {
        data->point.x = pt.x;
        data->point.y = pt.y;
        data->state = LV_INDEV_STATE_PRESSED;
        /* Signaler l'interaction au gestionnaire d'energie (feature 13). */
        if (ctx->notify_activity != NULL) {
            ctx->notify_activity();
        }
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

/* ================================================================
 * Tick LVGL via esp_timer
 * ================================================================ */

/**
 * Callback esp_timer : incremente le tick LVGL toutes les ms.
 */
static void lvgl_tick_cb(void *arg)
{
    (void)arg;
    lv_tick_inc(1);
}

/**
 * Demarre le timer periodique pour le tick LVGL.
 */
static void lvgl_tick_timer_start(void)
{
    const esp_timer_create_args_t timer_args = {
        .callback = lvgl_tick_cb,
        .name = "lvgl_tick",
    };
    esp_timer_handle_t timer;
    esp_timer_create(&timer_args, &timer);
    esp_timer_start_periodic(timer, 1000); /* 1ms en microsecondes */
}

/* ================================================================
 * Notification temporaire (toast)
 * ================================================================ */

/** Overlay de notification en cours d'affichage */
static lv_obj_t *s_toast = NULL;

/**
 * Callback du timer de notification : ferme le toast apres 2 secondes.
 */
static void toast_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    if (s_toast) {
        lv_obj_delete(s_toast);
        s_toast = NULL;
    }
}

/**
 * Affiche une notification temporaire en haut de l'ecran.
 *
 * @param text  Texte a afficher
 * @param color Couleur de fond du toast
 */
static void show_toast(const char *text, lv_color_t color)
{
    /* Fermer un toast existant */
    if (s_toast) {
        lv_obj_delete(s_toast);
        s_toast = NULL;
    }

    lv_obj_t *scr = lv_screen_active();
    if (!scr) return;

    /* Creer un bandeau en haut de l'ecran */
    s_toast = lv_obj_create(scr);
    lv_obj_set_size(s_toast, lv_pct(90), 32);
    lv_obj_align(s_toast, LV_ALIGN_TOP_MID, 0, 4);
    lv_obj_set_style_bg_color(s_toast, color, 0);
    lv_obj_set_style_bg_opa(s_toast, LV_OPA_90, 0);
    lv_obj_set_style_radius(s_toast, 6, 0);
    lv_obj_set_style_border_width(s_toast, 0, 0);
    lv_obj_set_style_pad_all(s_toast, 4, 0);
    lv_obj_clear_flag(s_toast, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *lbl = lv_label_create(s_toast);
    lv_obj_set_style_text_font(lbl, ui_theme_font_normal(), 0);
    lv_obj_set_style_text_color(lbl, lv_color_hex(0xFFFFFF), 0);
    lv_label_set_text(lbl, text);
    lv_obj_center(lbl);

    /* Auto-fermeture apres 2 secondes */
    lv_timer_t *t = lv_timer_create(toast_timer_cb, 2000, NULL);
    lv_timer_set_repeat_count(t, 1);
}

/**
 * Verifie s'il y a un broadcast en attente et navigue vers l'ecran broadcast.
 *
 * Quand core_task recoit un broadcast, il met broadcast_pending a true
 * et copie le message dans pending_broadcast. La ui_task detecte ce flag
 * et affiche automatiquement l'ecran de notification broadcast.
 */
static void check_broadcast_pending(ui_ctx_t *ctx)
{
    if (!ctx->broadcast_pending) return;

    /* Lecture sous mutex pour eviter une race condition */
    if (xSemaphoreTake(ctx->state_mutex, pdMS_TO_TICKS(50)) != pdTRUE) {
        return;
    }

    bool pending = *ctx->broadcast_pending;
    xSemaphoreGive(ctx->state_mutex);

    if (!pending) return;

    /* Ne pas re-naviguer si on est deja sur l'ecran broadcast */
    if (ui_manager_current() == UI_SCREEN_BROADCAST) return;

    /* Rallumer le retroeclairage si l'ecran est en veille */
    ctx->display->set_backlight(80, ctx->display->ctx);

    /* Naviguer vers l'ecran broadcast */
    ESP_LOGI(TAG, "Broadcast recu : affichage ecran notification");
    ui_manager_show(UI_SCREEN_BROADCAST);
}

/**
 * Verifie s'il y a un feedback de paiement en attente et l'affiche.
 *
 * La lecture et le reset de pay_feedback sont proteges par le mutex
 * pour eviter une race condition avec core_task qui ecrit le feedback.
 */
static void check_pay_feedback(ui_ctx_t *ctx)
{
    if (!ctx->pay_feedback) return;

    /* Lecture et reset sous mutex pour eviter la race condition */
    if (xSemaphoreTake(ctx->state_mutex, pdMS_TO_TICKS(50)) != pdTRUE) {
        return;
    }

    ui_pay_feedback_t fb = *ctx->pay_feedback;
    if (fb != UI_PAY_FEEDBACK_NONE) {
        /* Reset immediat pour ne pas re-afficher */
        *ctx->pay_feedback = UI_PAY_FEEDBACK_NONE;
    }

    xSemaphoreGive(ctx->state_mutex);

    if (fb == UI_PAY_FEEDBACK_NONE) return;

    switch (fb) {
        case UI_PAY_FEEDBACK_OK:
            show_toast("Paiement envoye !", UI_COLOR_SUCCESS);
            break;
        case UI_PAY_FEEDBACK_NO_FUNDS:
            show_toast("Solde insuffisant", UI_COLOR_WARNING);
            break;
        case UI_PAY_FEEDBACK_FAIL:
            show_toast("Erreur paiement", lv_color_hex(0xCC3333));
            break;
        default:
            break;
    }
}

/* ================================================================
 * Tache FreeRTOS
 * ================================================================ */

void ui_task(void *pvParam)
{
    ui_ctx_t *ctx = (ui_ctx_t *)pvParam;

    ESP_LOGI(TAG, "Demarrage ui_task");

    /* 1. Initialiser LVGL */
    lv_init();
    ESP_LOGI(TAG, "LVGL initialise");

    /* 2. Demarrer le tick LVGL */
    lvgl_tick_timer_start();

    /* 3. Recuperer la resolution ecran */
    ctx->display->get_resolution(&ctx->screen_w, &ctx->screen_h,
                                  ctx->display->ctx);
    /* Detection petit ecran par la plus petite dimension.
     * CYD : 320x240 → min=240 → grand.
     * Waveshare paysage : 320x172 → min=172 → petit. */
    uint16_t min_dim = (ctx->screen_w < ctx->screen_h)
                     ? ctx->screen_w : ctx->screen_h;
    ctx->is_small_screen = (min_dim < 200);

    ESP_LOGI(TAG, "Ecran %ux%u (%s)",
             ctx->screen_w, ctx->screen_h,
             ctx->is_small_screen ? "petit" : "grand");

    /* 4. Creer le display LVGL */
    lv_display_t *disp = lv_display_create(ctx->screen_w, ctx->screen_h);

    /*
     * Allouer les draw buffers (double buffer, 1/10 de l'ecran) en RAM
     * interne DMA. Le flush LCD lit ces buffers via SPI DMA : eviter la
     * PSRAM supprime les surprises de coherence cache sur ESP32-S3.
     */
    size_t buf_size = (size_t)ctx->screen_w * (ctx->screen_h / 10)
                      * sizeof(lv_color16_t);

    uint8_t *buf1 = heap_caps_malloc(buf_size, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    uint8_t *buf2 = heap_caps_malloc(buf_size, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);

    if (!buf1 || !buf2) {
        ESP_LOGE(TAG, "Echec alloc draw buffers (%u octets x2)", buf_size);
        vTaskDelete(NULL);
        return;
    }

    lv_display_set_buffers(disp, buf1, buf2, buf_size,
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_user_data(disp, ctx);
    lv_display_set_flush_cb(disp, lvgl_flush_cb);

    ESP_LOGI(TAG, "Display LVGL cree (buf=%u octets x2)", buf_size);

    /* 5. Initialiser le display hardware */
    hal_err_t herr = ctx->display->init(ctx->display->ctx);
    if (herr != HAL_OK) {
        ESP_LOGW(TAG, "Init display HAL echoue (%d) — mode stub", herr);
    }
    ctx->display->set_backlight(80, ctx->display->ctx);

    /* 6. Creer l'input device LVGL (tactile) */
    lv_indev_t *indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_user_data(indev, ctx);
    lv_indev_set_read_cb(indev, lvgl_touch_read_cb);

    /* 7. Initialiser le theme et le gestionnaire d'ecrans */
    ui_theme_init(ctx->is_small_screen);
    ui_manager_init(ctx);

    /* 8. Afficher le premier ecran :
     *    - Si aucun PIN n'est configure → ecran SETUP (premier boot)
     *    - Sinon → ecran HOME
     */
    if (!ui_pin_is_configured(ctx->storage)) {
        ESP_LOGI(TAG, "Premier boot detecte : affichage ecran SETUP");
        ui_manager_show(UI_SCREEN_SETUP);
    } else {
        ui_manager_show(UI_SCREEN_HOME);
    }

    ESP_LOGI(TAG, "UI prete, entree dans la boucle de rendu");

    /* 9. Boucle principale : rendu LVGL + rafraichissement des donnees */
    uint32_t update_counter = 0;
    while (1) {
        uint32_t ms_till_next = lv_timer_handler();

        /*
         * Rafraichir les donnees de l'ecran toutes les ~500ms
         * pour ne pas surcharger le mutex.
         * lv_timer_handler retourne typiquement 5-33ms.
         */
        update_counter++;
        if (update_counter >= 100) { /* ~500ms a 5ms/iteration */
            ui_manager_update();
            check_pay_feedback(ctx);
            check_broadcast_pending(ctx);
            update_counter = 0;
        }

        vTaskDelay(pdMS_TO_TICKS(ms_till_next < 5 ? 5 : ms_till_next));
    }
}
