/* Date & Time page.
 *
 * The panel has no SNTP, so this is the only way the clock gets set. Six
 * rollers rather than a text field: on a 3.5" resistive-feel panel a roller
 * is far easier to hit than a keyboard, and it cannot produce an unparseable
 * string.
 *
 * Everything here is LOCAL time -- the wall clock the installer reads off
 * their phone. app_time_set_local() converts through the configured timezone
 * and stores UTC in the RTC.
 */
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "app_events.h"
#include "app_settings.h"
#include "app_time.h"
#include "ui_internal.h"

#define YEAR_FIRST   2020
#define YEAR_COUNT   30

typedef enum {
    FIELD_YEAR = 0,
    FIELD_MONTH,
    FIELD_DAY,
    FIELD_HOUR,
    FIELD_MIN,
    FIELD_SEC,
    FIELD_COUNT,
} dt_field_t;

static const char *k_captions[FIELD_COUNT] = {
    "Year", "Month", "Day", "Hour", "Min", "Sec",
};
static const lv_coord_t k_widths[FIELD_COUNT] = { 76, 60, 60, 60, 60, 60 };

static lv_obj_t   *s_roller[FIELD_COUNT];
static lv_obj_t   *s_now;
static lv_obj_t   *s_rtc_pill;
static lv_timer_t *s_timer;

/* Roller option strings have to outlive the call that sets them, so they are
 * built once into static storage rather than onto the stack. */
static char s_opts_year[YEAR_COUNT * 5 + 1];
static char s_opts_month[12 * 3 + 1];
static char s_opts_day[31 * 3 + 1];
static char s_opts_hour[24 * 3 + 1];
static char s_opts_minsec[60 * 3 + 1];

/* --------------------------------------------------------------------------
 * Option strings
 * ------------------------------------------------------------------------ */
static void build_numeric_options(char *dst, size_t dst_len, int first, int count,
                                  const char *fmt)
{
    size_t pos = 0;
    for (int i = 0; i < count && pos < dst_len; i++) {
        pos += (size_t)snprintf(dst + pos, dst_len - pos, fmt, first + i);
        if (i + 1 < count && pos < dst_len) {
            dst[pos++] = '\n';
            dst[pos]   = '\0';
        }
    }
}

/** The day roller only offers days the selected month actually has. */
static void refresh_day_options(void)
{
    const int year  = YEAR_FIRST + (int)lv_roller_get_selected(s_roller[FIELD_YEAR]);
    const int month = (int)lv_roller_get_selected(s_roller[FIELD_MONTH]) + 1;
    const int days  = app_time_days_in_month(year, month);

    const uint16_t was = lv_roller_get_selected(s_roller[FIELD_DAY]);

    build_numeric_options(s_opts_day, sizeof(s_opts_day), 1, days, "%02d");
    lv_roller_set_options(s_roller[FIELD_DAY], s_opts_day, LV_ROLLER_MODE_NORMAL);

    /* Going from the 31st to February must land on the 28th/29th, not wrap. */
    lv_roller_set_selected(s_roller[FIELD_DAY],
                           was < (uint16_t)days ? was : (uint16_t)(days - 1),
                           LV_ANIM_OFF);
}

static void month_or_year_changed(lv_event_t *e)
{
    (void)e;
    refresh_day_options();
}

/* --------------------------------------------------------------------------
 * Load / save
 * ------------------------------------------------------------------------ */
static void load_from_clock(void)
{
    struct tm now;
    app_time_get_local(&now);

    int year = now.tm_year + 1900;
    if (year < YEAR_FIRST)                 year = YEAR_FIRST;
    if (year > YEAR_FIRST + YEAR_COUNT - 1) year = YEAR_FIRST + YEAR_COUNT - 1;

    lv_roller_set_selected(s_roller[FIELD_YEAR],  (uint16_t)(year - YEAR_FIRST), LV_ANIM_OFF);
    lv_roller_set_selected(s_roller[FIELD_MONTH], (uint16_t)now.tm_mon,          LV_ANIM_OFF);
    refresh_day_options();
    lv_roller_set_selected(s_roller[FIELD_DAY],   (uint16_t)(now.tm_mday - 1),   LV_ANIM_OFF);
    lv_roller_set_selected(s_roller[FIELD_HOUR],  (uint16_t)now.tm_hour,         LV_ANIM_OFF);
    lv_roller_set_selected(s_roller[FIELD_MIN],   (uint16_t)now.tm_min,          LV_ANIM_OFF);
    lv_roller_set_selected(s_roller[FIELD_SEC],   (uint16_t)now.tm_sec,          LV_ANIM_OFF);
}

static void reload_clicked(lv_event_t *e)
{
    (void)e;
    load_from_clock();
    ui_toast("Reset to the current clock");
}

static void save_clicked(lv_event_t *e)
{
    (void)e;

    struct tm local = {0};
    local.tm_year = YEAR_FIRST + (int)lv_roller_get_selected(s_roller[FIELD_YEAR]) - 1900;
    local.tm_mon  = (int)lv_roller_get_selected(s_roller[FIELD_MONTH]);
    local.tm_mday = (int)lv_roller_get_selected(s_roller[FIELD_DAY]) + 1;
    local.tm_hour = (int)lv_roller_get_selected(s_roller[FIELD_HOUR]);
    local.tm_min  = (int)lv_roller_get_selected(s_roller[FIELD_MIN]);
    local.tm_sec  = (int)lv_roller_get_selected(s_roller[FIELD_SEC]);

    if (app_time_set_local(&local) != ESP_OK) {
        ui_toast("Could not set the clock");
        return;
    }

    ui_toast(app_time_has_rtc() ? "Clock set and saved to the RTC"
                                : "Clock set (no RTC -- lost on power off)");
    ui_back();
}

/* --------------------------------------------------------------------------
 * Live readout
 * ------------------------------------------------------------------------ */
static void tick(lv_timer_t *t)
{
    (void)t;

    char buf[48];
    app_time_format(buf, sizeof(buf), "%Y-%m-%d  %H:%M:%S");

    char line[64];
    snprintf(line, sizeof(line), "Now:  %s", buf);
    lv_label_set_text(s_now, line);
}

/* --------------------------------------------------------------------------
 * Layout
 * ------------------------------------------------------------------------ */
static void create(lv_obj_t *parent)
{
    const lv_coord_t w = ui_width() - 2 * UI_PAD;

    build_numeric_options(s_opts_year,   sizeof(s_opts_year),   YEAR_FIRST, YEAR_COUNT, "%04d");
    build_numeric_options(s_opts_month,  sizeof(s_opts_month),  1,  12, "%02d");
    build_numeric_options(s_opts_day,    sizeof(s_opts_day),    1,  31, "%02d");
    build_numeric_options(s_opts_hour,   sizeof(s_opts_hour),   0,  24, "%02d");
    build_numeric_options(s_opts_minsec, sizeof(s_opts_minsec), 0,  60, "%02d");

    /* ---- current clock ---- */
    lv_obj_t *head = ui_card(parent, w, 40);
    lv_obj_align(head, LV_ALIGN_TOP_MID, 0, 0);

    s_now = ui_label(head, "Now:  -------:--", &lv_font_montserrat_16, UI_COL_TEXT);
    lv_obj_align(s_now, LV_ALIGN_LEFT_MID, 2, 0);

    s_rtc_pill = ui_pill(head, "RTC", UI_COL_PRIMARY);
    lv_obj_align(s_rtc_pill, LV_ALIGN_RIGHT_MID, 0, 0);

    /* ---- rollers ---- */
    lv_obj_t *card = ui_card(parent, w, 168);
    lv_obj_align(card, LV_ALIGN_TOP_MID, 0, 46);
    lv_obj_set_style_pad_all(card, 6, 0);

    lv_coord_t total = 0;
    for (int i = 0; i < FIELD_COUNT; i++) {
        total += k_widths[i];
    }
    const lv_coord_t gap  = 7;
    total += gap * (FIELD_COUNT - 1);
    lv_coord_t x = (w - 12 - total) / 2;

    for (int i = 0; i < FIELD_COUNT; i++) {
        lv_obj_t *cap = ui_label(card, k_captions[i], &lv_font_montserrat_12, UI_COL_MUTED);
        lv_obj_set_width(cap, k_widths[i]);
        lv_obj_set_style_text_align(cap, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_pos(cap, x, 0);

        lv_obj_t *r = lv_roller_create(card);
        switch (i) {
        case FIELD_YEAR:  lv_roller_set_options(r, s_opts_year,   LV_ROLLER_MODE_NORMAL); break;
        case FIELD_MONTH: lv_roller_set_options(r, s_opts_month,  LV_ROLLER_MODE_NORMAL); break;
        case FIELD_DAY:   lv_roller_set_options(r, s_opts_day,    LV_ROLLER_MODE_NORMAL); break;
        case FIELD_HOUR:  lv_roller_set_options(r, s_opts_hour,   LV_ROLLER_MODE_NORMAL); break;
        default:          lv_roller_set_options(r, s_opts_minsec, LV_ROLLER_MODE_NORMAL); break;
        }

        lv_roller_set_visible_row_count(r, 3);
        lv_obj_set_width(r, k_widths[i]);
        lv_obj_set_pos(r, x, 18);
        lv_obj_set_style_text_font(r, &lv_font_montserrat_16, 0);
        lv_obj_set_style_bg_color(r, UI_COL_PRIMARY, LV_PART_SELECTED);
        lv_obj_set_style_text_color(r, lv_color_white(), LV_PART_SELECTED);
        lv_obj_set_style_border_width(r, 1, 0);
        lv_obj_set_style_border_color(r, UI_COL_TRACK, 0);
        lv_obj_set_style_radius(r, 6, 0);

        s_roller[i] = r;
        x += k_widths[i] + gap;
    }

    /* Rebuilding the day list has to happen after every roller exists. */
    lv_obj_add_event_cb(s_roller[FIELD_YEAR],  month_or_year_changed,
                        LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(s_roller[FIELD_MONTH], month_or_year_changed,
                        LV_EVENT_VALUE_CHANGED, NULL);

    /* ---- footer ---- */
    lv_obj_t *hint = ui_label(parent,
                              "Enter local time. The clock is set by hand; there is no "
                              "network sync.",
                              &lv_font_montserrat_12, UI_COL_MUTED);
    lv_obj_align(hint, LV_ALIGN_TOP_LEFT, 2, 220);

    lv_obj_t *reload = ui_button_soft(parent, LV_SYMBOL_REFRESH " Current",
                                      reload_clicked, NULL);
    lv_obj_set_size(reload, 130, 34);
    lv_obj_align(reload, LV_ALIGN_BOTTOM_LEFT, 0, 0);

    lv_obj_t *save = ui_button(parent, LV_SYMBOL_OK " Set Clock", save_clicked, NULL);
    lv_obj_set_size(save, 150, 34);
    lv_obj_align(save, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
}

static void on_enter(void)
{
    ui_pill_set(s_rtc_pill,
                app_time_has_rtc() ? "RTC" : "no RTC",
                app_time_has_rtc() ? UI_COL_PRIMARY : UI_COL_WARN);

    load_from_clock();

    if (!s_timer) {
        s_timer = lv_timer_create(tick, 1000, NULL);
    }
    lv_timer_resume(s_timer);
    tick(NULL);
}

static void on_leave(void)
{
    if (s_timer) {
        lv_timer_pause(s_timer);
    }
}

const ui_screen_def_t ui_screen_datetime_def = {
    .title    = "Date & Time",
    .create   = create,
    .on_enter = on_enter,
    .on_leave = on_leave,
};
