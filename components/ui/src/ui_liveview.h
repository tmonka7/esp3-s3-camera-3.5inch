/* The video surface shared by the Home preview, the Camera page, the
 * Detection page and Playback.
 *
 * It owns an lv_img bound to whichever RGB565 buffer the media service last
 * published, plus the green detection box drawn over it.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ui_liveview ui_liveview_t;

/** Creates the surface. `w`/`h` are the on-screen size; frames are scaled
 *  to fit and centred, so any sensor mode looks right. */
ui_liveview_t *ui_liveview_create(lv_obj_t *parent, lv_coord_t w, lv_coord_t h);

lv_obj_t *ui_liveview_obj(ui_liveview_t *lv);

/** Routes camera frames here and starts the preview. */
void ui_liveview_attach_camera(ui_liveview_t *lv);

/** Routes AVI playback frames here instead. */
void ui_liveview_attach_player(ui_liveview_t *lv);

/** Stops delivery. Always call this from the screen's on_leave. */
void ui_liveview_detach(ui_liveview_t *lv);

/** Draws (or hides) the detection box. Coordinates are per mille of frame. */
void ui_liveview_set_box(ui_liveview_t *lv, bool visible, const char *label,
                         uint16_t x, uint16_t y, uint16_t w, uint16_t h);

/** Overlay text in the top-left corner, e.g. the timestamp. */
void ui_liveview_set_caption(ui_liveview_t *lv, const char *text);

/** Shown centred while no frames are arriving. */
void ui_liveview_set_placeholder(ui_liveview_t *lv, const char *text);

#ifdef __cplusplus
}
#endif
