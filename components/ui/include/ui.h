#pragma once

#include <stdbool.h>
#include "lvgl.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    UI_SCREEN_SPLASH = 0,
    UI_SCREEN_HOME,
    UI_SCREEN_CAMERA,
    UI_SCREEN_UART,
    UI_SCREEN_MODBUS,
    UI_SCREEN_POWER,
    UI_SCREEN_SWITCHES,
    UI_SCREEN_TEMP,
    UI_SCREEN_DETECT,
    UI_SCREEN_STORAGE,
    UI_SCREEN_PLAYBACK,
    UI_SCREEN_SETTINGS,
    UI_SCREEN_DATETIME,
    UI_SCREEN_ACCESS,
    UI_SCREEN_CARDS,
    UI_SCREEN_FACES,
    UI_SCREEN_WEBCAM,
    UI_SCREEN_COUNT,
} ui_screen_id_t;

/**
 * One screen module.
 *
 * `create` builds the content into `parent` and is called once, lazily, the
 * first time the screen is shown -- twelve screens built up front would cost
 * more RAM than the panel has to spare. `on_enter` / `on_leave` start and
 * stop whatever the screen needs running while it is visible (the camera
 * pump, the player, a refresh timer).
 */
typedef struct {
    const char *title;
    /** Splash and Home draw their own chrome, so they get the bare screen
     *  object instead of a content area below the standard header. */
    bool   full_bleed;
    void  (*create)(lv_obj_t *parent);
    void  (*on_enter)(void);
    void  (*on_leave)(void);
} ui_screen_def_t;

/** Builds the splash screen and subscribes the UI to the event bus. */
esp_err_t ui_init(void);

/** Switches screens, running the leave/enter hooks. Safe from any task. */
void ui_show(ui_screen_id_t id);

/** Returns to the previous screen, or Home when there is none. */
void ui_back(void);

ui_screen_id_t ui_current_screen(void);

/** Drives the splash progress bar during start-up. */
void ui_splash_progress(int percent, const char *message);

/** Transient message strip at the bottom of the current screen. */
void ui_toast(const char *fmt, ...);

#ifdef __cplusplus
}
#endif
