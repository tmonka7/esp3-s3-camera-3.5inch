#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app_events.h"
#include "bsp_storage.h"
#include "media_files.h"
#include "media_player.h"
#include "ui_internal.h"
#include "ui_liveview.h"

#define LIST_ROWS  4

static ui_liveview_t *s_view;
static lv_obj_t      *s_rows[LIST_ROWS];
static lv_obj_t      *s_slider;
static lv_obj_t      *s_pos;
static lv_obj_t      *s_dur;
static lv_obj_t      *s_play_icon;
static lv_obj_t      *s_pill;
static lv_timer_t    *s_timer;

static media_entry_t *s_clips;
static size_t         s_clip_count;
static int            s_scroll;
static int            s_selected = -1;
/* Set while the user is dragging, so the periodic refresh does not fight the
 * slider back to the playhead mid-gesture. */
static bool           s_scrubbing;

/* --------------------------------------------------------------------------
 * Clip list
 * ------------------------------------------------------------------------ */
static void set_row(int i, const media_entry_t *e, bool selected)
{
    lv_obj_t *row = s_rows[i];
    if (!row) {
        return;
    }

    if (!e) {
        lv_obj_add_flag(row, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    lv_obj_clear_flag(row, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_bg_color(row, selected ? UI_COL_PRIMARY_SOFT
                                            : lv_color_hex(0xF7F9F8), 0);

    lv_obj_t *title = lv_obj_get_child(row, 1);
    lv_obj_t *sub   = lv_obj_get_child(row, 2);

    /* "2026-09-22_14-35-07.avi" reads better split into date and time. */
    char name[MEDIA_NAME_LEN];
    strlcpy(name, e->name, sizeof(name));
    char *dot = strrchr(name, '.');
    if (dot) {
        *dot = '\0';
    }
    lv_label_set_text(title, name);

    char size[24];
    media_files_format_size(e->size, size, sizeof(size));
    lv_label_set_text(sub, size);
}

static void refresh_list(void)
{
    if (!s_clips) {
        return;
    }

    s_clip_count = media_files_list(MEDIA_KIND_VIDEO, s_clips, MEDIA_MAX_ENTRIES);

    if (s_scroll > (int)s_clip_count - LIST_ROWS) {
        s_scroll = (int)s_clip_count - LIST_ROWS;
    }
    if (s_scroll < 0) {
        s_scroll = 0;
    }

    for (int i = 0; i < LIST_ROWS; i++) {
        const int idx = s_scroll + i;
        set_row(i, (idx < (int)s_clip_count) ? &s_clips[idx] : NULL, idx == s_selected);
    }
}

static void open_clip(int index)
{
    if (index < 0 || index >= (int)s_clip_count) {
        return;
    }

    char path[192];
    media_files_path(&s_clips[index], path, sizeof(path));

    if (media_player_open(path) == ESP_OK) {
        s_selected = index;
        refresh_list();
        ui_pill_set(s_pill, "Playing", UI_COL_PRIMARY);
    } else {
        ui_toast("Cannot play %s", s_clips[index].name);
    }
}

static void row_clicked(lv_event_t *e)
{
    open_clip(s_scroll + (int)(intptr_t)lv_event_get_user_data(e));
}

static void scroll_clicked(lv_event_t *e)
{
    s_scroll += (int)(intptr_t)lv_event_get_user_data(e);
    refresh_list();
}

/* --------------------------------------------------------------------------
 * Transport controls
 * ------------------------------------------------------------------------ */
static void toggle_play(lv_event_t *e)
{
    (void)e;
    media_player_toggle();
}

static void step(lv_event_t *e)
{
    media_player_step((int)(intptr_t)lv_event_get_user_data(e));
}

static void slider_event(lv_event_t *e)
{
    const lv_event_code_t code = lv_event_get_code(e);

    if (code == LV_EVENT_PRESSED) {
        s_scrubbing = true;
    } else if (code == LV_EVENT_VALUE_CHANGED) {
        media_player_seek_permille((uint16_t)lv_slider_get_value(s_slider));
    } else if (code == LV_EVENT_RELEASED) {
        s_scrubbing = false;
    }
}

static void tick(lv_timer_t *t)
{
    (void)t;

    player_status_t st;
    media_player_status(&st);

    char buf[16];
    snprintf(buf, sizeof(buf), "%02lu:%02lu",
             (unsigned long)(st.position_s / 60), (unsigned long)(st.position_s % 60));
    lv_label_set_text(s_pos, buf);

    snprintf(buf, sizeof(buf), "%02lu:%02lu",
             (unsigned long)(st.duration_s / 60), (unsigned long)(st.duration_s % 60));
    lv_label_set_text(s_dur, buf);

    if (!s_scrubbing && st.frame_count > 0) {
        const int32_t pos = (int32_t)(((uint64_t)st.frame * 1000u) / st.frame_count);
        lv_slider_set_value(s_slider, pos, LV_ANIM_OFF);
    }

    lv_label_set_text(s_play_icon,
                      st.state == PLAYER_PLAYING ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);

    if (st.state == PLAYER_STOPPED) {
        ui_pill_set(s_pill, "Stopped", UI_COL_MUTED);
    } else if (st.state == PLAYER_PAUSED) {
        ui_pill_set(s_pill, "Paused", UI_COL_WARN);
    }
}

/* --------------------------------------------------------------------------
 * Layout
 * ------------------------------------------------------------------------ */
static void create(lv_obj_t *parent)
{
    const lv_coord_t w = ui_width() - 2 * UI_PAD;
    const lv_coord_t h = ui_content_height() - 2 * UI_PAD;

    const lv_coord_t list_w   = 168;
    const lv_coord_t view_w   = w - list_w - UI_PAD;
    const lv_coord_t ctrl_h   = 52;

    /* ---- video surface ---- */
    s_view = ui_liveview_create(parent, view_w, h - ctrl_h - UI_PAD);
    lv_obj_align(ui_liveview_obj(s_view), LV_ALIGN_TOP_LEFT, 0, 0);
    ui_liveview_set_placeholder(s_view, "Pick a recording");

    /* ---- transport ---- */
    lv_obj_t *ctrl = ui_card(parent, view_w, ctrl_h);
    lv_obj_set_pos(ctrl, 0, h - ctrl_h);
    lv_obj_set_style_pad_all(ctrl, 5, 0);

    s_pos = ui_label(ctrl, "00:00", &lv_font_montserrat_12, UI_COL_MUTED);
    lv_obj_align(s_pos, LV_ALIGN_TOP_LEFT, 2, 0);

    s_dur = ui_label(ctrl, "00:00", &lv_font_montserrat_12, UI_COL_MUTED);
    lv_obj_align(s_dur, LV_ALIGN_TOP_RIGHT, -2, 0);

    s_slider = lv_slider_create(ctrl);
    lv_obj_set_size(s_slider, view_w - 24, 5);
    lv_obj_align(s_slider, LV_ALIGN_TOP_MID, 0, 16);
    lv_slider_set_range(s_slider, 0, 1000);
    lv_obj_set_style_bg_color(s_slider, UI_COL_TRACK, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_slider, UI_COL_PRIMARY, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_slider, UI_COL_PRIMARY, LV_PART_KNOB);
    lv_obj_add_event_cb(s_slider, slider_event, LV_EVENT_PRESSED,       NULL);
    lv_obj_add_event_cb(s_slider, slider_event, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(s_slider, slider_event, LV_EVENT_RELEASED,      NULL);

    lv_obj_t *prev = ui_button_soft(ctrl, LV_SYMBOL_PREV, step, (void *)(intptr_t)-15);
    lv_obj_set_size(prev, 34, 24);
    lv_obj_align(prev, LV_ALIGN_BOTTOM_MID, -46, 0);

    lv_obj_t *play = ui_button(ctrl, "", toggle_play, NULL);
    lv_obj_set_size(play, 40, 24);
    lv_obj_align(play, LV_ALIGN_BOTTOM_MID, 0, 0);
    s_play_icon = lv_obj_get_child(play, 0);
    lv_label_set_text(s_play_icon, LV_SYMBOL_PLAY);

    lv_obj_t *next = ui_button_soft(ctrl, LV_SYMBOL_NEXT, step, (void *)(intptr_t)15);
    lv_obj_set_size(next, 34, 24);
    lv_obj_align(next, LV_ALIGN_BOTTOM_MID, 46, 0);

    /* ---- clip list ---- */
    lv_obj_t *list = ui_card(parent, list_w, h);
    lv_obj_align(list, LV_ALIGN_TOP_RIGHT, 0, 0);
    lv_obj_set_style_pad_all(list, 5, 0);
    ui_flex_col(list, 3);

    for (int i = 0; i < LIST_ROWS; i++) {
        lv_obj_t *row = lv_btn_create(list);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, LV_PCT(100), 38);
        lv_obj_set_style_bg_color(row, lv_color_hex(0xF7F9F8), 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(row, 6, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(row, LV_OBJ_FLAG_HIDDEN);

        lv_obj_t *icon = ui_label(row, LV_SYMBOL_PLAY, &lv_font_montserrat_14,
                                  UI_COL_PRIMARY);
        lv_obj_align(icon, LV_ALIGN_LEFT_MID, 5, 0);

        lv_obj_t *title = ui_label(row, "", &lv_font_montserrat_12, UI_COL_TEXT);
        lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
        lv_obj_set_width(title, list_w - 46);
        lv_obj_align(title, LV_ALIGN_LEFT_MID, 26, -7);

        lv_obj_t *sub = ui_label(row, "", &lv_font_montserrat_12, UI_COL_MUTED);
        lv_obj_align(sub, LV_ALIGN_LEFT_MID, 26, 8);

        lv_obj_add_event_cb(row, row_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        s_rows[i] = row;
    }

    lv_obj_t *nav = lv_obj_create(list);
    lv_obj_remove_style_all(nav);
    lv_obj_set_size(nav, LV_PCT(100), 26);
    lv_obj_clear_flag(nav, LV_OBJ_FLAG_SCROLLABLE);
    ui_flex_row(nav, 6);

    lv_obj_set_size(ui_button_soft(nav, LV_SYMBOL_UP, scroll_clicked,
                                   (void *)(intptr_t)-LIST_ROWS), 60, 24);
    lv_obj_set_size(ui_button_soft(nav, LV_SYMBOL_DOWN, scroll_clicked,
                                   (void *)(intptr_t)LIST_ROWS), 60, 24);
}

static void on_enter(void)
{
    lv_obj_t *slot = ui_header_slot(UI_SCREEN_PLAYBACK);
    if (slot && !s_pill) {
        s_pill = ui_pill(slot, "Stopped", UI_COL_MUTED);
    }

    if (!s_clips) {
        s_clips = calloc(MEDIA_MAX_ENTRIES, sizeof(media_entry_t));
    }
    s_selected = -1;
    refresh_list();

    ui_liveview_attach_player(s_view);

    if (!s_timer) {
        s_timer = lv_timer_create(tick, 400, NULL);
    }
    lv_timer_resume(s_timer);
}

static void on_leave(void)
{
    if (s_timer) {
        lv_timer_pause(s_timer);
    }

    media_player_stop();
    ui_liveview_detach(s_view);

    free(s_clips);
    s_clips      = NULL;
    s_clip_count = 0;
}

const ui_screen_def_t ui_screen_playback_def = {
    .title    = "Video Playback",
    .create   = create,
    .on_enter = on_enter,
    .on_leave = on_leave,
};
