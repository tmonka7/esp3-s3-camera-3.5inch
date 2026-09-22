#include <stdio.h>
#include <string.h>

#include "app_events.h"
#include "svc_power.h"
#include "ui_internal.h"

#define RAIL_COUNT  POWER_RAIL_COUNT
/* The chart shows a quarter of the stored day so the shape stays readable on
 * a 200 px wide plot. */
#define CHART_POINTS 24

static const char *k_rail_names[RAIL_COUNT] = {
    "Main Power", "Solar Inverter", "Battery", "UPS",
};
static const char *k_rail_icons[RAIL_COUNT] = {
    LV_SYMBOL_HOME, LV_SYMBOL_EYE_OPEN, LV_SYMBOL_BATTERY_FULL, LV_SYMBOL_CHARGE,
};

static lv_obj_t      *s_pill;
static lv_obj_t      *s_total;
static lv_obj_t      *s_voltage;
static lv_obj_t      *s_current;
static lv_obj_t      *s_energy;
static lv_obj_t      *s_rail_sw[RAIL_COUNT];
static lv_obj_t      *s_chart;
static lv_chart_series_t *s_series;

static void rail_toggled(lv_event_t *e)
{
    lv_obj_t          *sw   = lv_event_get_target(e);
    const power_rail_t rail = (power_rail_t)(uintptr_t)lv_event_get_user_data(e);
    const bool         on   = lv_obj_has_state(sw, LV_STATE_CHECKED);

    if (svc_power_set_rail(rail, on) != ESP_OK) {
        /* Snap back rather than leave the switch showing a state the
         * hardware never reached. */
        if (on) {
            lv_obj_clear_state(sw, LV_STATE_CHECKED);
        } else {
            lv_obj_add_state(sw, LV_STATE_CHECKED);
        }
        ui_toast("%s did not respond", k_rail_names[rail]);
    }
}

static void refresh_chart(void)
{
    uint32_t points[CHART_POINTS];
    const size_t n = svc_power_history(points, CHART_POINTS);

    for (size_t i = 0; i < CHART_POINTS; i++) {
        const lv_coord_t v = (i < n) ? (lv_coord_t)(points[i] / 10) : 0;   /* 10 W units */
        lv_chart_set_value_by_id(s_chart, s_series, (uint16_t)i, v);
    }
    lv_chart_refresh(s_chart);
}

static void apply_status(const power_status_t *st)
{
    char buf[32];

    if (st->valid) {
        snprintf(buf, sizeof(buf), "%lu.%02lu kW",
                 (unsigned long)(st->power_w / 1000),
                 (unsigned long)((st->power_w % 1000) / 10));
    } else {
        snprintf(buf, sizeof(buf), "--.-- kW");
    }
    lv_label_set_text(s_total, buf);

    snprintf(buf, sizeof(buf), "%u.%u V", st->voltage_v10 / 10, st->voltage_v10 % 10);
    lv_label_set_text(s_voltage, buf);

    snprintf(buf, sizeof(buf), "%u.%02u A", st->current_a100 / 100, st->current_a100 % 100);
    lv_label_set_text(s_current, buf);

    snprintf(buf, sizeof(buf), "%lu.%02lu kWh",
             (unsigned long)(st->energy_wh / 1000),
             (unsigned long)((st->energy_wh % 1000) / 10));
    lv_label_set_text(s_energy, buf);

    for (int i = 0; i < RAIL_COUNT; i++) {
        if (st->rail[i]) {
            lv_obj_add_state(s_rail_sw[i], LV_STATE_CHECKED);
        } else {
            lv_obj_clear_state(s_rail_sw[i], LV_STATE_CHECKED);
        }
    }

    ui_pill_set(s_pill, st->rail[POWER_RAIL_MAIN] ? "Power ON" : "Power OFF",
                st->rail[POWER_RAIL_MAIN] ? UI_COL_PRIMARY : UI_COL_MUTED);
}

static void on_power(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base; (void)id;

    const power_status_t *st = data;
    if (!st || !ui_lock()) {
        return;
    }
    apply_status(st);
    refresh_chart();
    ui_unlock();
}

/* --------------------------------------------------------------------------
 * Layout
 * ------------------------------------------------------------------------ */
static lv_obj_t *stat_box(lv_obj_t *parent, const char *caption, lv_coord_t x,
                          lv_coord_t w, lv_obj_t **out_value)
{
    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, w, 40);
    lv_obj_set_pos(box, x, 0);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *cap = ui_label(box, caption, &lv_font_montserrat_12, UI_COL_MUTED);
    lv_obj_align(cap, LV_ALIGN_TOP_LEFT, 0, 0);

    *out_value = ui_label(box, "--", &lv_font_montserrat_16, UI_COL_TEXT);
    lv_obj_align(*out_value, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    return box;
}

static void create(lv_obj_t *parent)
{
    const lv_coord_t w = ui_width() - 2 * UI_PAD;
    const lv_coord_t h = ui_content_height() - 2 * UI_PAD;

    /* ---- summary strip ---- */
    lv_obj_t *summary = ui_card(parent, w, 62);
    lv_obj_align(summary, LV_ALIGN_TOP_MID, 0, 0);

    lv_obj_t *bolt = ui_label(summary, LV_SYMBOL_CHARGE, &lv_font_montserrat_24,
                              lv_color_white());
    lv_obj_set_style_bg_color(bolt, UI_COL_PRIMARY, 0);
    lv_obj_set_style_bg_opa(bolt, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(bolt, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_all(bolt, 8, 0);
    lv_obj_align(bolt, LV_ALIGN_LEFT_MID, 0, 0);

    lv_obj_t *tcap = ui_label(summary, "Total Power", &lv_font_montserrat_12, UI_COL_MUTED);
    lv_obj_align(tcap, LV_ALIGN_LEFT_MID, 50, -12);

    s_total = ui_label(summary, "--.-- kW", &lv_font_montserrat_24, UI_COL_TEXT);
    lv_obj_align(s_total, LV_ALIGN_LEFT_MID, 50, 8);

    lv_obj_t *stats = lv_obj_create(summary);
    lv_obj_remove_style_all(stats);
    lv_obj_set_size(stats, w - 200, 40);
    lv_obj_align(stats, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_clear_flag(stats, LV_OBJ_FLAG_SCROLLABLE);

    const lv_coord_t sw = (w - 200) / 3;
    stat_box(stats, "Voltage", 0,        sw, &s_voltage);
    stat_box(stats, "Current", sw,       sw, &s_current);
    stat_box(stats, "Energy",  sw * 2,   sw, &s_energy);

    /* ---- rails + chart ---- */
    const lv_coord_t bottom_y = 62 + UI_PAD;
    const lv_coord_t bottom_h = h - bottom_y;
    const lv_coord_t rails_w  = 176;

    lv_obj_t *rails = ui_card(parent, rails_w, bottom_h);
    lv_obj_set_pos(rails, 0, bottom_y);
    ui_flex_col(rails, 4);

    for (int i = 0; i < RAIL_COUNT; i++) {
        lv_obj_t *row = lv_obj_create(rails);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, LV_PCT(100), 30);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *icon = ui_label(row, k_rail_icons[i], &lv_font_montserrat_14,
                                  UI_COL_PRIMARY);
        lv_obj_align(icon, LV_ALIGN_LEFT_MID, 2, 0);

        lv_obj_t *name = ui_label(row, k_rail_names[i], &lv_font_montserrat_12,
                                  UI_COL_TEXT);
        lv_obj_align(name, LV_ALIGN_LEFT_MID, 24, 0);

        s_rail_sw[i] = lv_switch_create(row);
        lv_obj_set_size(s_rail_sw[i], 42, 22);
        lv_obj_align(s_rail_sw[i], LV_ALIGN_RIGHT_MID, 0, 0);
        lv_obj_set_style_bg_color(s_rail_sw[i], UI_COL_PRIMARY,
                                  LV_PART_INDICATOR | LV_STATE_CHECKED);
        lv_obj_add_event_cb(s_rail_sw[i], rail_toggled, LV_EVENT_VALUE_CHANGED,
                            (void *)(uintptr_t)i);
    }

    lv_obj_t *chart_card = ui_card(parent, w - rails_w - UI_PAD, bottom_h);
    lv_obj_set_pos(chart_card, rails_w + UI_PAD, bottom_y);

    ui_card_title(chart_card, "Power Usage");

    s_chart = lv_chart_create(chart_card);
    lv_obj_set_size(s_chart, w - rails_w - UI_PAD - 20, bottom_h - 38);
    lv_obj_align(s_chart, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_chart_set_type(s_chart, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(s_chart, CHART_POINTS);
    lv_chart_set_update_mode(s_chart, LV_CHART_UPDATE_MODE_SHIFT);
    lv_chart_set_div_line_count(s_chart, 3, 0);
    lv_obj_set_style_border_width(s_chart, 0, 0);
    lv_obj_set_style_bg_opa(s_chart, LV_OPA_TRANSP, 0);
    lv_obj_set_style_size(s_chart, 0, LV_PART_INDICATOR);   /* hide point dots */

    s_series = lv_chart_add_series(s_chart, UI_COL_PRIMARY, LV_CHART_AXIS_PRIMARY_Y);
    /* Values are stored in 10 W units, so 300 is 3 kW full scale. */
    lv_chart_set_range(s_chart, LV_CHART_AXIS_PRIMARY_Y, 0, 300);
}

static void on_enter(void)
{
    lv_obj_t *slot = ui_header_slot(UI_SCREEN_POWER);
    if (slot && !s_pill) {
        s_pill = ui_pill(slot, "Power OFF", UI_COL_MUTED);
    }

    power_status_t st;
    svc_power_get(&st);
    apply_status(&st);
    refresh_chart();

    app_event_subscribe(APP_EVT_POWER_UPDATE, on_power, NULL);
}

static void on_leave(void)
{
    app_event_unsubscribe(APP_EVT_POWER_UPDATE, on_power);
}

const ui_screen_def_t ui_screen_power_def = {
    .title    = "Power Control",
    .create   = create,
    .on_enter = on_enter,
    .on_leave = on_leave,
};
