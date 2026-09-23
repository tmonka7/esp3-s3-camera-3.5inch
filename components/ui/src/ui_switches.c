#include <stdio.h>
#include <string.h>

#include "app_events.h"
#include "app_settings.h"
#include "svc_switch.h"
#include "ui_internal.h"

#define COLS  5
#define ROWS  2

typedef struct {
    lv_obj_t *card;
    lv_obj_t *icon;
    lv_obj_t *name;
    lv_obj_t *pill;
} switch_cell_t;

static switch_cell_t s_cells[APP_SWITCH_COUNT];

static void apply_cell(uint8_t i, bool on, bool ok)
{
    switch_cell_t *c = &s_cells[i];
    if (!c->card) {
        return;
    }

    ui_pill_set(c->pill, on ? UI_T(ON_UPPER) : UI_T(OFF_UPPER),
                !ok ? UI_COL_DANGER : (on ? UI_COL_PRIMARY : UI_COL_MUTED));

    lv_obj_set_style_text_color(c->icon, on ? UI_COL_PRIMARY : UI_COL_TRACK, 0);
    lv_obj_set_style_border_color(c->card, on ? UI_COL_PRIMARY : UI_COL_TRACK, 0);
    lv_obj_set_style_border_width(c->card, on ? 2 : 1, 0);
}

static void cell_clicked(lv_event_t *e)
{
    const uint8_t i = (uint8_t)(uintptr_t)lv_event_get_user_data(e);

    /* The service publishes APP_EVT_SWITCH_UPDATE, which repaints the cell --
     * including when the write failed. Nothing is painted optimistically. */
    if (svc_switch_toggle(i) != ESP_OK) {
        ui_toast(UI_T(SW_NO_REPLY_FMT), svc_switch_name(i));
    }
}

static void on_update(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base; (void)id;

    const switch_update_t *u = data;
    if (!u || u->index >= APP_SWITCH_COUNT || !ui_lock()) {
        return;
    }
    apply_cell(u->index, u->state, u->ok);
    ui_unlock();
}

static void create(lv_obj_t *parent)
{
    const lv_coord_t w = ui_width() - 2 * UI_PAD;
    const lv_coord_t h = ui_content_height() - 2 * UI_PAD;

    const lv_coord_t gap    = 6;
    const lv_coord_t cell_w = (w - (COLS - 1) * gap) / COLS;
    const lv_coord_t cell_h = (h - (ROWS - 1) * gap) / ROWS;

    const app_settings_t *cfg = app_settings();

    for (uint8_t i = 0; i < APP_SWITCH_COUNT; i++) {
        switch_cell_t *c = &s_cells[i];

        c->card = ui_card(parent, cell_w, cell_h);
        lv_obj_set_pos(c->card,
                       (lv_coord_t)(i % COLS) * (cell_w + gap),
                       (lv_coord_t)(i / COLS) * (cell_h + gap));
        lv_obj_set_style_border_width(c->card, 1, 0);
        lv_obj_set_style_border_color(c->card, UI_COL_TRACK, 0);
        lv_obj_set_style_pad_all(c->card, 4, 0);
        lv_obj_add_flag(c->card, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(c->card, cell_clicked, LV_EVENT_CLICKED,
                            (void *)(uintptr_t)i);

        char num[4];
        snprintf(num, sizeof(num), "%u", (unsigned)(i + 1));
        lv_obj_t *idx = ui_label(c->card, num, UI_FONT_12, UI_COL_MUTED);
        lv_obj_align(idx, LV_ALIGN_TOP_MID, 0, 0);

        c->icon = ui_label(c->card, LV_SYMBOL_POWER, UI_FONT_24, UI_COL_TRACK);
        lv_obj_align(c->icon, LV_ALIGN_TOP_MID, 0, 16);

        c->name = ui_label(c->card, cfg->switches[i].name,
                           UI_FONT_12, UI_COL_TEXT);
        lv_label_set_long_mode(c->name, LV_LABEL_LONG_DOT);
        lv_obj_set_width(c->name, cell_w - 10);
        lv_obj_set_style_text_align(c->name, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(c->name, LV_ALIGN_BOTTOM_MID, 0, -22);

        c->pill = ui_pill(c->card, UI_T(OFF_UPPER), UI_COL_MUTED);
        lv_obj_align(c->pill, LV_ALIGN_BOTTOM_MID, 0, 0);
    }
}

static void on_enter(void)
{
    for (uint8_t i = 0; i < APP_SWITCH_COUNT; i++) {
        apply_cell(i, svc_switch_get(i), true);
    }

    /* Changes other controllers make on the bus arrive through this event;
     * the switch service polls for them on its own task, because the reads
     * block for as long as the Modbus response timeout and this runs under
     * the LVGL lock. */
    app_event_subscribe(APP_EVT_SWITCH_UPDATE, on_update, NULL);
}

static void on_leave(void)
{
    app_event_unsubscribe(APP_EVT_SWITCH_UPDATE, on_update);
}

const ui_screen_def_t ui_screen_switches_def = {
    .title    = UI_STR_TITLE_SWITCHES,
    .create   = create,
    .on_enter = on_enter,
    .on_leave = on_leave,
};
