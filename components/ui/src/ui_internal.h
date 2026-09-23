/* Shared between the UI modules only. Not part of the component's API. */
#pragma once

#include "ui.h"
#include "ui_theme.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Every screen module defines exactly one of these. */
extern const ui_screen_def_t ui_screen_splash_def;
extern const ui_screen_def_t ui_screen_home_def;
extern const ui_screen_def_t ui_screen_camera_def;
extern const ui_screen_def_t ui_screen_uart_def;
extern const ui_screen_def_t ui_screen_modbus_def;
extern const ui_screen_def_t ui_screen_power_def;
extern const ui_screen_def_t ui_screen_switches_def;
extern const ui_screen_def_t ui_screen_temp_def;
extern const ui_screen_def_t ui_screen_detect_def;
extern const ui_screen_def_t ui_screen_storage_def;
extern const ui_screen_def_t ui_screen_playback_def;
extern const ui_screen_def_t ui_screen_settings_def;
extern const ui_screen_def_t ui_screen_datetime_def;

/**
 * The right-hand slot of the standard header, where screens park their
 * status pill. NULL for full-bleed screens.
 */
lv_obj_t *ui_header_slot(ui_screen_id_t id);

/** Screen width and height, after rotation. */
lv_coord_t ui_width(void);
lv_coord_t ui_height(void);

/** Content area height, i.e. the screen minus the standard header. */
lv_coord_t ui_content_height(void);

/**
 * Screens subscribe to the app event bus in on_enter and unsubscribe in
 * on_leave, so only the visible screen does any work. Handlers run on the
 * event-loop task, so they must wrap widget access in these.
 */
bool ui_lock(void);
void ui_unlock(void);

#ifdef __cplusplus
}
#endif
