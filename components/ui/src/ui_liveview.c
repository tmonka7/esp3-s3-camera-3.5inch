#include <string.h>
#include <stdlib.h>

#include "esp_log.h"

#include "svc_media.h"
#include "media_player.h"
#include "ui_internal.h"
#include "ui_liveview.h"

static const char *TAG = "ui_live";

struct ui_liveview {
    lv_obj_t     *root;         /* clipping container   */
    lv_obj_t     *img;
    lv_obj_t     *box;          /* detection rectangle  */
    lv_obj_t     *box_label;
    lv_obj_t     *caption;
    lv_obj_t     *placeholder;
    lv_img_dsc_t  dsc;
    lv_coord_t    view_w, view_h;
    uint16_t      frame_w, frame_h;
    bool          attached;
};

/* Only one surface can be receiving frames, because the media service holds a
 * single preview callback. Tracking it here lets a screen switch re-point
 * delivery without the outgoing screen having to co-operate. */
static ui_liveview_t *s_active;

/* --------------------------------------------------------------------------
 * Frame delivery (runs on the media pump / player task)
 * ------------------------------------------------------------------------ */
static void on_frame(const uint8_t *rgb565, int w, int h, void *ctx)
{
    ui_liveview_t *lv = ctx;
    if (!lv || !rgb565 || w <= 0 || h <= 0) {
        return;
    }

    /* Taking the LVGL lock here is what makes the media service's double
     * buffering safe: while we hold it LVGL cannot be rendering, so swapping
     * the pointer can never tear a frame in progress. */
    if (!ui_lock()) {
        return;
    }

    /* Only reachable while this surface is the active one. */
    if (s_active == lv) {
        lv->dsc.header.always_zero = 0;
        lv->dsc.header.w           = (uint32_t)w;
        lv->dsc.header.h           = (uint32_t)h;
        lv->dsc.header.cf          = LV_IMG_CF_TRUE_COLOR;
        lv->dsc.data_size          = (uint32_t)w * (uint32_t)h * 2u;
        lv->dsc.data               = rgb565;

        if (lv->frame_w != (uint16_t)w || lv->frame_h != (uint16_t)h) {
            lv->frame_w = (uint16_t)w;
            lv->frame_h = (uint16_t)h;

            /* Scale to fit, keeping the aspect ratio. 256 is 1:1 in LVGL. */
            const int zoom_w = (int)lv->view_w * 256 / w;
            const int zoom_h = (int)lv->view_h * 256 / h;
            int       zoom   = (zoom_w < zoom_h) ? zoom_w : zoom_h;
            if (zoom < 16)  zoom = 16;
            if (zoom > 1024) zoom = 1024;

            lv_img_set_zoom(lv->img, (uint16_t)zoom);
            lv_obj_set_size(lv->img, w, h);
            lv_obj_center(lv->img);
        }

        lv_img_set_src(lv->img, &lv->dsc);
        lv_obj_clear_flag(lv->img, LV_OBJ_FLAG_HIDDEN);
        lv_obj_invalidate(lv->img);

        if (lv->placeholder && !lv_obj_has_flag(lv->placeholder, LV_OBJ_FLAG_HIDDEN)) {
            lv_obj_add_flag(lv->placeholder, LV_OBJ_FLAG_HIDDEN);
        }
    }

    ui_unlock();
}

/* --------------------------------------------------------------------------
 * Construction
 * ------------------------------------------------------------------------ */
ui_liveview_t *ui_liveview_create(lv_obj_t *parent, lv_coord_t w, lv_coord_t h)
{
    ui_liveview_t *lv = calloc(1, sizeof(ui_liveview_t));
    if (!lv) {
        return NULL;
    }

    lv->view_w = w;
    lv->view_h = h;

    lv->root = lv_obj_create(parent);
    lv_obj_remove_style_all(lv->root);
    lv_obj_set_size(lv->root, w, h);
    lv_obj_set_style_bg_color(lv->root, lv_color_hex(0x101418), 0);
    lv_obj_set_style_bg_opa(lv->root, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(lv->root, 6, 0);
    lv_obj_set_style_clip_corner(lv->root, true, 0);
    lv_obj_clear_flag(lv->root, LV_OBJ_FLAG_SCROLLABLE);

    lv->img = lv_img_create(lv->root);
    lv_obj_center(lv->img);
    /* Nearest-neighbour: the scaling is cosmetic and antialiasing would cost
     * more than it is worth at 10-20 fps on this panel. */
    lv_img_set_antialias(lv->img, false);

    lv->placeholder = ui_label(lv->root, UI_T(LIVE_CAMERA_OFF), UI_FONT_14,
                               lv_color_hex(0x8A94A0));
    lv_obj_center(lv->placeholder);

    lv->caption = ui_label(lv->root, "", UI_FONT_12, lv_color_white());
    lv_obj_align(lv->caption, LV_ALIGN_TOP_LEFT, 6, 5);
    lv_obj_set_style_bg_color(lv->caption, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(lv->caption, LV_OPA_50, 0);
    lv_obj_set_style_pad_all(lv->caption, 2, 0);
    lv_obj_set_style_radius(lv->caption, 3, 0);

    /* Detection box: an outline only, so it never hides what it marks. */
    lv->box = lv_obj_create(lv->root);
    lv_obj_remove_style_all(lv->box);
    lv_obj_set_style_border_color(lv->box, UI_COL_PRIMARY, 0);
    lv_obj_set_style_border_width(lv->box, 2, 0);
    lv_obj_set_style_radius(lv->box, 3, 0);
    lv_obj_set_style_bg_opa(lv->box, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(lv->box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(lv->box, LV_OBJ_FLAG_HIDDEN);

    lv->box_label = ui_label(lv->root, "", UI_FONT_12, lv_color_white());
    lv_obj_set_style_bg_color(lv->box_label, UI_COL_PRIMARY, 0);
    lv_obj_set_style_bg_opa(lv->box_label, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_hor(lv->box_label, 4, 0);
    lv_obj_set_style_pad_ver(lv->box_label, 1, 0);
    lv_obj_add_flag(lv->box_label, LV_OBJ_FLAG_HIDDEN);

    return lv;
}

lv_obj_t *ui_liveview_obj(ui_liveview_t *lv)
{
    return lv ? lv->root : NULL;
}

void ui_liveview_attach_camera(ui_liveview_t *lv)
{
    if (!lv) {
        return;
    }
    s_active = lv;
    lv->attached = true;

    media_player_set_frame_cb(NULL, NULL);
    svc_media_set_preview_cb(on_frame, lv);
    svc_media_start_preview();
}

void ui_liveview_attach_player(ui_liveview_t *lv)
{
    if (!lv) {
        return;
    }
    s_active = lv;
    lv->attached = true;

    svc_media_stop_preview();
    svc_media_set_preview_cb(NULL, NULL);
    media_player_set_frame_cb(on_frame, lv);
}

void ui_liveview_detach(ui_liveview_t *lv)
{
    if (!lv || s_active != lv) {
        return;
    }

    svc_media_stop_preview();
    svc_media_set_preview_cb(NULL, NULL);
    media_player_set_frame_cb(NULL, NULL);

    s_active     = NULL;
    lv->attached = false;

    /* Stop referencing a buffer the media service is free to reuse. Hiding
     * the widget is enough -- passing NULL to lv_img_set_src() would leave
     * the decoder holding the old descriptor. */
    lv->dsc.data = NULL;
    lv->frame_w  = 0;
    lv->frame_h  = 0;
    lv_obj_add_flag(lv->img, LV_OBJ_FLAG_HIDDEN);

    if (lv->placeholder) {
        lv_obj_clear_flag(lv->placeholder, LV_OBJ_FLAG_HIDDEN);
    }
}

void ui_liveview_set_box(ui_liveview_t *lv, bool visible, const char *label,
                         uint16_t x, uint16_t y, uint16_t w, uint16_t h)
{
    if (!lv) {
        return;
    }

    if (!visible) {
        lv_obj_add_flag(lv->box, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(lv->box_label, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    /* Per-mille coordinates map onto the visible surface, not the frame, so
     * the box stays aligned whatever scaling the image ended up with. */
    const lv_coord_t bx = (lv_coord_t)((int32_t)lv->view_w * x / 1000);
    const lv_coord_t by = (lv_coord_t)((int32_t)lv->view_h * y / 1000);
    const lv_coord_t bw = (lv_coord_t)((int32_t)lv->view_w * w / 1000);
    const lv_coord_t bh = (lv_coord_t)((int32_t)lv->view_h * h / 1000);

    lv_obj_set_pos(lv->box, bx, by);
    lv_obj_set_size(lv->box, bw > 4 ? bw : 4, bh > 4 ? bh : 4);
    lv_obj_clear_flag(lv->box, LV_OBJ_FLAG_HIDDEN);

    if (label && label[0]) {
        lv_label_set_text(lv->box_label, label);
        lv_obj_set_pos(lv->box_label, bx, by > 14 ? by - 14 : by);
        lv_obj_clear_flag(lv->box_label, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(lv->box_label, LV_OBJ_FLAG_HIDDEN);
    }
}

void ui_liveview_set_caption(ui_liveview_t *lv, const char *text)
{
    if (lv && lv->caption) {
        lv_label_set_text(lv->caption, text ? text : "");
    }
}

void ui_liveview_set_placeholder(ui_liveview_t *lv, const char *text)
{
    if (lv && lv->placeholder) {
        lv_label_set_text(lv->placeholder, text ? text : "");
    }
}
