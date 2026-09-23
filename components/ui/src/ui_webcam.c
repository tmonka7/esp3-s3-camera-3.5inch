/* The web stream page.
 *
 * Its whole job is to answer "what do I type into a browser, and is anything
 * actually coming out" -- so the URL is the biggest thing on it, and the
 * local preview is there to confirm the panel is capturing at all when the
 * stream looks black at the other end.
 */
#include <stdio.h>
#include <string.h>

#include "app_events.h"
#include "app_net.h"
#include "app_settings.h"
#include "svc_webcam.h"
#include "ui_internal.h"
#include "ui_liveview.h"

static ui_liveview_t *s_view;
static lv_obj_t      *s_url;
static lv_obj_t      *s_hint;
static lv_obj_t      *s_stats;
static lv_obj_t      *s_toggle;
static lv_obj_t      *s_pill;
static lv_timer_t    *s_tick;

/* --------------------------------------------------------------------------
 * Painting
 * ------------------------------------------------------------------------ */
static void paint(void)
{
    if (!s_url) {
        return;
    }

    const bool running = svc_webcam_running();

    char url[72];
    svc_webcam_url(url, sizeof(url));
    lv_label_set_text(s_url, url);
    lv_obj_set_style_text_color(s_url,
                                running ? UI_COL_TEXT : UI_COL_MUTED, 0);

    if (!app_net_is_connected()) {
        lv_label_set_text(s_hint,
                          "Wi-Fi is not connected.\n"
                          "Settings > Network, then come back.");
    } else if (!running) {
        lv_label_set_text(s_hint,
                          "Start the server, then open the address\n"
                          "in a browser or VLC on the same network.");
    } else if (svc_webcam_streaming()) {
        lv_label_set_text(s_hint, "A viewer is connected.\n"
                                  "One at a time; a second is refused.");
    } else {
        lv_label_set_text(s_hint, "Waiting for a viewer.\n"
                                  "/stream is MJPEG, /jpg is a single frame.");
    }

    char stats[48];
    snprintf(stats, sizeof(stats), "%u frames sent",
             (unsigned)svc_webcam_frames_sent());
    lv_label_set_text(s_stats, stats);

    lv_label_set_text(lv_obj_get_child(s_toggle, 0),
                      running ? "Stop server" : "Start server");

    if (!running) {
        ui_pill_set(s_pill, "Stopped", UI_COL_MUTED);
    } else if (svc_webcam_streaming()) {
        ui_pill_set(s_pill, "Streaming", UI_COL_PRIMARY);
    } else {
        ui_pill_set(s_pill, "Listening", UI_COL_INFO);
    }
}

/* --------------------------------------------------------------------------
 * Actions
 * ------------------------------------------------------------------------ */
static void toggle_clicked(lv_event_t *e)
{
    (void)e;

    app_settings_t *cfg = app_settings();

    if (svc_webcam_running()) {
        svc_webcam_stop();
        cfg->webcam_enabled = false;
    } else if (svc_webcam_start() == ESP_OK) {
        cfg->webcam_enabled = true;
        if (!app_net_is_connected()) {
            /* The server is genuinely up, it just has no address yet; saying
             * so beats showing a URL that cannot be reached. */
            ui_toast("Started -- waiting for Wi-Fi");
        }
    } else {
        ui_toast("Could not start the server");
        return;
    }

    app_settings_commit();
    paint();
}

static void port_applied(int value)
{
    app_settings_t *cfg = app_settings();
    const bool      was = svc_webcam_running();

    cfg->webcam_port = (uint16_t)value;
    app_settings_commit();

    /* The port is fixed when the listener is created, so a change only takes
     * effect after a bounce. */
    if (was) {
        svc_webcam_stop();
        svc_webcam_start();
    }
}

static void port_clicked(lv_event_t *e)
{
    (void)e;
    ui_edit_number("Stream port", 1, 65535,
                   app_settings()->webcam_port ? app_settings()->webcam_port : 81,
                   port_applied, paint);
}

/* --------------------------------------------------------------------------
 * Build
 * ------------------------------------------------------------------------ */
static void tick(lv_timer_t *t)
{
    (void)t;
    /* Nothing here publishes an event -- a viewer connecting happens inside
     * the HTTP server -- so the page polls. */
    paint();
}

static void create(lv_obj_t *parent)
{
    const lv_coord_t h       = ui_content_height() - 2 * UI_PAD;
    const lv_coord_t left_w  = ui_width() - 2 * UI_PAD - 176 - UI_PAD;
    const lv_coord_t right_w = 176;

    lv_obj_t *info = ui_card(parent, left_w, h);
    lv_obj_align(info, LV_ALIGN_TOP_LEFT, 0, 0);

    ui_card_title(info, "Stream address");

    s_url = ui_label(info, "", &lv_font_montserrat_16, UI_COL_TEXT);
    lv_obj_set_width(s_url, left_w - 24);
    lv_label_set_long_mode(s_url, LV_LABEL_LONG_WRAP);
    lv_obj_align(s_url, LV_ALIGN_TOP_LEFT, 0, 30);

    s_hint = ui_label(info, "", &lv_font_montserrat_12, UI_COL_MUTED);
    lv_obj_set_width(s_hint, left_w - 24);
    lv_label_set_long_mode(s_hint, LV_LABEL_LONG_WRAP);
    lv_obj_align(s_hint, LV_ALIGN_TOP_LEFT, 0, 66);

    s_stats = ui_label(info, "", &lv_font_montserrat_12, UI_COL_MUTED);
    lv_obj_align(s_stats, LV_ALIGN_TOP_LEFT, 0, 112);

    s_toggle = ui_button(info, "Start server", toggle_clicked, NULL);
    lv_obj_set_size(s_toggle, 150, 38);
    lv_obj_align(s_toggle, LV_ALIGN_BOTTOM_LEFT, 0, -4);

    lv_obj_t *port = ui_button_soft(info, "Port", port_clicked, NULL);
    lv_obj_set_size(port, 90, 38);
    lv_obj_align(port, LV_ALIGN_BOTTOM_LEFT, 158, -4);

    /* ---- what is being served ---- */
    lv_obj_t *cam = ui_card(parent, right_w, h);
    lv_obj_align(cam, LV_ALIGN_TOP_RIGHT, 0, 0);

    s_view = ui_liveview_create(cam, right_w - 20, h - 20);
    lv_obj_center(ui_liveview_obj(s_view));
    ui_liveview_set_placeholder(s_view, "Camera off");

    s_pill = ui_pill(ui_header_slot(UI_SCREEN_WEBCAM), "Stopped", UI_COL_MUTED);
}

static void on_enter(void)
{
    paint();
    ui_liveview_attach_camera(s_view);

    if (!s_tick) {
        s_tick = lv_timer_create(tick, 1000, NULL);
    }
}

static void on_leave(void)
{
    /* Detaching stops the local preview only. The server keeps running and
     * keeps its own frame subscription, which is the point -- you navigate
     * away and the stream carries on. */
    ui_liveview_detach(s_view);

    if (s_tick) {
        lv_timer_del(s_tick);
        s_tick = NULL;
    }
}

const ui_screen_def_t ui_screen_webcam_def = {
    .title    = "Web Stream",
    .create   = create,
    .on_enter = on_enter,
    .on_leave = on_leave,
};
