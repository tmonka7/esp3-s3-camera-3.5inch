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
extern const ui_screen_def_t ui_screen_access_def;
extern const ui_screen_def_t ui_screen_cards_def;

/* ---- modal edit dialogs (ui_dialog.c) ---------------------------------- *
 *
 * `apply` receives the edited value. `on_done` runs afterwards and is where
 * the screen rebuilds whatever the edit changed -- passing it keeps the
 * dialog from having to know which screen opened it.
 */
typedef void (*ui_apply_number_t)(int value);
typedef void (*ui_apply_text_t)(const char *text);

/** Dimmed overlay with a titled card. Returns the card, ready to fill. */
lv_obj_t *ui_dialog_shell(const char *title, lv_coord_t height);

/** Tears the modal down. Usable directly as an lv_event_cb_t. */
void ui_dialog_close(lv_event_t *e);

void ui_edit_number(const char *title, int min, int max, int current,
                    ui_apply_number_t apply, void (*on_done)(void));
void ui_edit_text(const char *title, const char *current, bool password,
                  ui_apply_text_t apply, void (*on_done)(void));

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
