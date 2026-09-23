#include <stdio.h>

#include "app_events.h"
#include "bsp_rfid.h"
#include "svc_access.h"
#include "ui_internal.h"

#define ENROLL_WINDOW_S   30

static lv_obj_t  *s_state_lbl;
static lv_obj_t  *s_count_lbl;
static lv_obj_t  *s_log_list;
static lv_obj_t  *s_pill;
static lv_timer_t *s_tick;

/* --------------------------------------------------------------------------
 * Painting
 * ------------------------------------------------------------------------ */
static void paint_lock(void)
{
    if (!s_state_lbl) {
        return;
    }

    const bool unlocked = svc_access_lock_state() == ACCESS_LOCK_UNLOCKED;

    lv_label_set_text(s_state_lbl, unlocked ? "UNLOCKED" : "LOCKED");
    lv_obj_set_style_text_color(s_state_lbl,
                                unlocked ? UI_COL_PRIMARY : UI_COL_TEXT, 0);

    const uint32_t left = svc_access_relock_in();
    if (unlocked && left) {
        char buf[32];
        snprintf(buf, sizeof(buf), "relocks in %u s", (unsigned)left);
        lv_label_set_text(s_count_lbl, buf);
    } else {
        lv_label_set_text(s_count_lbl, svc_access_enrolling()
                                           ? "waiting for a card..."
                                           : "");
    }
}

/** One line in the recent-decisions list. */
static void add_log_row(const access_event_t *e)
{
    lv_obj_t *row = lv_obj_create(s_log_list);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), 30);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    const bool good = (e->result == ACCESS_GRANTED ||
                       e->result == ACCESS_GRANTED_MANUAL ||
                       e->result == ACCESS_ENROLLED);

    lv_obj_t *dot = ui_label(row, LV_SYMBOL_BULLET, &lv_font_montserrat_14,
                             good ? UI_COL_PRIMARY : UI_COL_DANGER);
    lv_obj_align(dot, LV_ALIGN_LEFT_MID, 0, 0);

    lv_obj_t *time_lbl = ui_label(row, e->stamp, &lv_font_montserrat_12, UI_COL_MUTED);
    lv_obj_align(time_lbl, LV_ALIGN_LEFT_MID, 16, 0);

    /* Name when we know it, the raw credential when we do not -- an unknown
     * card is exactly the case where the UID is the useful thing to show. */
    char text[48];
    snprintf(text, sizeof(text), "%s  %s",
             svc_access_result_name(e->result),
             e->name[0] ? e->name : e->credential);

    lv_obj_t *what = ui_label(row, text, &lv_font_montserrat_12,
                              good ? UI_COL_TEXT : UI_COL_DANGER);
    lv_obj_align(what, LV_ALIGN_LEFT_MID, 74, 0);

    if (e->kind == ACCESS_CRED_FACE) {
        lv_obj_t *tag = ui_label(row, "face", &lv_font_montserrat_12, UI_COL_MUTED);
        lv_obj_align(tag, LV_ALIGN_RIGHT_MID, 0, 0);
    }
}

static void paint_log(void)
{
    if (!s_log_list) {
        return;
    }
    lv_obj_clean(s_log_list);

    access_event_t events[ACCESS_HISTORY_DEPTH];
    const size_t   n = svc_access_history(events, ACCESS_HISTORY_DEPTH);

    if (n == 0) {
        lv_obj_t *empty = ui_label(s_log_list, "Nothing yet",
                                   &lv_font_montserrat_12, UI_COL_MUTED);
        lv_obj_set_width(empty, LV_PCT(100));
        return;
    }

    for (size_t i = 0; i < n; i++) {
        add_log_row(&events[i]);
    }
}

static void paint_pill(void)
{
    if (!s_pill) {
        return;
    }
    if (bsp_rfid_present()) {
        ui_pill_set(s_pill, "Reader", UI_COL_PRIMARY);
    } else {
        ui_pill_set(s_pill, "No reader", UI_COL_MUTED);
    }
}

/* --------------------------------------------------------------------------
 * Actions
 * ------------------------------------------------------------------------ */
static void unlock_clicked(lv_event_t *e)
{
    (void)e;
    if (svc_access_unlock_manual() != ESP_OK) {
        ui_toast("Could not unlock");
    }
}

static void lock_clicked(lv_event_t *e)
{
    (void)e;
    svc_access_lock_now();
}

static void enroll_clicked(lv_event_t *e)
{
    (void)e;
    if (svc_access_enroll_begin(ENROLL_WINDOW_S) == ESP_OK) {
        ui_toast("Present a card within %d s", ENROLL_WINDOW_S);
        paint_lock();
    } else {
        ui_toast("No reader detected");
    }
}

static void cards_clicked(lv_event_t *e)
{
    (void)e;
    ui_show(UI_SCREEN_CARDS);
}

/* --------------------------------------------------------------------------
 * Events
 * ------------------------------------------------------------------------ */
static void on_access(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base; (void)id;

    const access_event_t *e = data;
    if (!e || !ui_lock()) {
        return;
    }
    paint_log();
    paint_lock();
    ui_unlock();

    if (e->result == ACCESS_ENROLLED) {
        ui_toast("Enrolled: %s", e->name);
    } else if (e->result == ACCESS_DENIED_LOCKOUT) {
        ui_toast("Too many failures -- reader paused");
    }
}

static void on_lock_state(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base; (void)id; (void)data;

    if (!ui_lock()) {
        return;
    }
    paint_lock();
    ui_unlock();
}

/** Drives the relock countdown and the enrolment hint. */
static void tick(lv_timer_t *t)
{
    (void)t;
    paint_lock();
}

/* --------------------------------------------------------------------------
 * Build
 * ------------------------------------------------------------------------ */
static void create(lv_obj_t *parent)
{
    const lv_coord_t h        = ui_content_height() - 2 * UI_PAD;
    const lv_coord_t left_w   = 172;
    const lv_coord_t right_w  = ui_width() - 2 * UI_PAD - left_w - UI_PAD;

    /* ---- left: the door itself ---- */
    lv_obj_t *door = ui_card(parent, left_w, h);
    lv_obj_align(door, LV_ALIGN_TOP_LEFT, 0, 0);

    ui_card_title(door, "Door");

    s_state_lbl = ui_label(door, "LOCKED", &lv_font_montserrat_24, UI_COL_TEXT);
    lv_obj_align(s_state_lbl, LV_ALIGN_TOP_MID, 0, 46);

    s_count_lbl = ui_label(door, "", &lv_font_montserrat_12, UI_COL_MUTED);
    lv_obj_align(s_count_lbl, LV_ALIGN_TOP_MID, 0, 78);

    lv_obj_t *unlock_btn = ui_button(door, "Unlock", unlock_clicked, NULL);
    lv_obj_set_size(unlock_btn, left_w - 28, 40);
    lv_obj_align(unlock_btn, LV_ALIGN_TOP_MID, 0, 104);

    lv_obj_t *lock_btn = ui_button_soft(door, "Lock now", lock_clicked, NULL);
    lv_obj_set_size(lock_btn, left_w - 28, 34);
    lv_obj_align(lock_btn, LV_ALIGN_TOP_MID, 0, 150);

    lv_obj_t *add_btn = ui_button_soft(door, LV_SYMBOL_PLUS " Add card",
                                       enroll_clicked, NULL);
    lv_obj_set_size(add_btn, left_w - 28, 34);
    lv_obj_align(add_btn, LV_ALIGN_BOTTOM_MID, 0, -42);

    lv_obj_t *cards_btn = ui_button_soft(door, LV_SYMBOL_LIST " Cards",
                                         cards_clicked, NULL);
    lv_obj_set_size(cards_btn, left_w - 28, 34);
    lv_obj_align(cards_btn, LV_ALIGN_BOTTOM_MID, 0, -4);

    /* ---- right: who has been at the door ---- */
    lv_obj_t *log = ui_card(parent, right_w, h);
    lv_obj_align(log, LV_ALIGN_TOP_RIGHT, 0, 0);

    ui_card_title(log, "Recent");

    s_log_list = lv_obj_create(log);
    lv_obj_remove_style_all(s_log_list);
    lv_obj_set_size(s_log_list, right_w - 24, h - 46);
    lv_obj_align(s_log_list, LV_ALIGN_BOTTOM_MID, 0, -8);
    ui_flex_col(s_log_list, 0);
    lv_obj_set_style_pad_all(s_log_list, 0, 0);

    s_pill = ui_pill(ui_header_slot(UI_SCREEN_ACCESS), "No reader", UI_COL_MUTED);
}

static void on_enter(void)
{
    paint_pill();
    paint_lock();
    paint_log();

    app_event_subscribe(APP_EVT_ACCESS, on_access, NULL);
    app_event_subscribe(APP_EVT_LOCK_STATE, on_lock_state, NULL);

    /* The relock countdown is the only thing here that changes without an
     * event to announce it. */
    if (!s_tick) {
        s_tick = lv_timer_create(tick, 1000, NULL);
    }
}

static void on_leave(void)
{
    app_event_unsubscribe(APP_EVT_ACCESS, on_access);
    app_event_unsubscribe(APP_EVT_LOCK_STATE, on_lock_state);

    if (s_tick) {
        lv_timer_del(s_tick);
        s_tick = NULL;
    }
    /* Leaving the page closes the enrolment window: an "add card" mode left
     * running unattended is how strangers' cards get enrolled. */
    svc_access_enroll_cancel();
}

const ui_screen_def_t ui_screen_access_def = {
    .title    = "Door Access",
    .create   = create,
    .on_enter = on_enter,
    .on_leave = on_leave,
};
