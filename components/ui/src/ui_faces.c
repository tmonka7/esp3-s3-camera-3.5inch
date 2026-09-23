/* Enrolling and managing faces.
 *
 * Enrolment is deliberately not a single snapshot: the recogniser takes five
 * shots a few hundred milliseconds apart, so the stored reference covers a
 * little natural movement instead of one frozen pose. The live view is here
 * so you can see what the camera sees while it does that -- an enrolment made
 * off-centre or backlit is the usual reason recognition never works
 * afterwards.
 */
#include <stdio.h>
#include <string.h>

#include "app_events.h"
#include "svc_face.h"
#include "ui_internal.h"
#include "ui_liveview.h"

static ui_liveview_t *s_view;
static lv_obj_t      *s_status;
static lv_obj_t      *s_bar;
static lv_obj_t      *s_enrol_btn;
static lv_obj_t      *s_list;
static lv_obj_t      *s_pill;
static bool           s_was_enrolling;

static void build_list(void);

/* --------------------------------------------------------------------------
 * Enrolment
 * ------------------------------------------------------------------------ */
static void apply_name(const char *text)
{
    if (!text || !text[0]) {
        return;
    }

    switch (svc_face_enroll_begin(text)) {
    case ESP_OK:
        s_was_enrolling = true;
        ui_toast("Look at the camera");
        break;
    case ESP_ERR_NO_MEM:
        ui_toast("No room for another face");
        break;
    default:
        ui_toast("Recogniser is not running");
        break;
    }
}

static void enrol_clicked(lv_event_t *e)
{
    (void)e;

    if (svc_face_enrolling()) {
        svc_face_enroll_cancel();
        s_was_enrolling = false;
        ui_toast("Cancelled");
        return;
    }
    ui_edit_text("Whose face is this?", "", false, apply_name, NULL);
}

static void delete_clicked(lv_event_t *e)
{
    const size_t index = (size_t)(uintptr_t)lv_event_get_user_data(e);

    if (svc_face_subject_remove(index) == ESP_OK) {
        ui_toast("Removed");
        build_list();
    } else {
        ui_toast("Could not remove");
    }
}

/* --------------------------------------------------------------------------
 * Painting
 * ------------------------------------------------------------------------ */
static void paint_status(const face_status_t *st)
{
    if (!s_status) {
        return;
    }

    const bool enrolling = svc_face_enrolling();
    char       text[64];

    if (enrolling) {
        const uint8_t got = svc_face_enroll_progress();
        if (st && st->face_found) {
            snprintf(text, sizeof(text), "Hold still  -  %u of %d",
                     (unsigned)got, FACE_SAMPLES_PER);
        } else {
            snprintf(text, sizeof(text), "No face in view  -  %u of %d",
                     (unsigned)got, FACE_SAMPLES_PER);
        }
        lv_bar_set_value(s_bar, got, LV_ANIM_ON);
    } else if (st && st->match_id) {
        snprintf(text, sizeof(text), "%s  (%u%%)", st->match_name,
                 (unsigned)st->confidence);
        lv_bar_set_value(s_bar, 0, LV_ANIM_OFF);
    } else if (st && st->face_found) {
        strlcpy(text, "Face seen, not recognised", sizeof(text));
        lv_bar_set_value(s_bar, 0, LV_ANIM_OFF);
    } else if (svc_face_is_enabled()) {
        strlcpy(text, "Watching", sizeof(text));
        lv_bar_set_value(s_bar, 0, LV_ANIM_OFF);
    } else {
        strlcpy(text, "Face unlock is off", sizeof(text));
        lv_bar_set_value(s_bar, 0, LV_ANIM_OFF);
    }

    lv_label_set_text(s_status, text);
    lv_label_set_text(lv_obj_get_child(s_enrol_btn, 0),
                      enrolling ? "Cancel" : LV_SYMBOL_PLUS " Enrol a face");
}

/** The box arrives in analysis-frame pixels; the widget wants per mille. */
static void paint_box(const face_status_t *st)
{
    if (!s_view) {
        return;
    }
    if (!st || !st->face_found || !st->frame_w || !st->frame_h) {
        ui_liveview_set_box(s_view, false, NULL, 0, 0, 0, 0);
        return;
    }

    ui_liveview_set_box(s_view, true,
                        st->match_id ? st->match_name : NULL,
                        (uint16_t)((uint32_t)st->x * 1000u / st->frame_w),
                        (uint16_t)((uint32_t)st->y * 1000u / st->frame_h),
                        (uint16_t)((uint32_t)st->w * 1000u / st->frame_w),
                        (uint16_t)((uint32_t)st->h * 1000u / st->frame_h));
}

static void add_row(size_t index, const face_subject_t *s)
{
    lv_obj_t *row = lv_obj_create(s_list);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), 42);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_color(row, UI_COL_TRACK, 0);
    lv_obj_set_style_border_width(row, 1, 0);

    lv_obj_t *name = ui_label(row, s->name, &lv_font_montserrat_14, UI_COL_TEXT);
    lv_obj_align(name, LV_ALIGN_TOP_LEFT, 4, 3);

    char detail[40];
    snprintf(detail, sizeof(detail), "%u shots", (unsigned)s->samples);
    lv_obj_t *sub = ui_label(row, detail, &lv_font_montserrat_12, UI_COL_MUTED);
    lv_obj_align(sub, LV_ALIGN_TOP_LEFT, 4, 22);

    lv_obj_t *bin = ui_button_soft(row, LV_SYMBOL_TRASH, delete_clicked,
                                   (void *)(uintptr_t)index);
    lv_obj_set_size(bin, 40, 30);
    lv_obj_align(bin, LV_ALIGN_RIGHT_MID, -4, 0);
    lv_obj_set_style_text_color(bin, UI_COL_DANGER, 0);
}

static void build_list(void)
{
    if (!s_list) {
        return;
    }
    lv_obj_clean(s_list);

    const size_t n = svc_face_subject_count();
    if (n == 0) {
        lv_obj_t *empty = ui_label(s_list,
                                   "Nobody enrolled.\n\n"
                                   "Enrol a face, then turn on Face Entry\n"
                                   "in Settings > Access.",
                                   &lv_font_montserrat_12, UI_COL_MUTED);
        lv_obj_set_width(empty, LV_PCT(100));
        return;
    }

    for (size_t i = 0; i < n; i++) {
        face_subject_t s;
        if (svc_face_subject_get(i, &s) == ESP_OK) {
            add_row(i, &s);
        }
    }
}

/* --------------------------------------------------------------------------
 * Events
 * ------------------------------------------------------------------------ */
static void on_face(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base; (void)id;

    const face_status_t *st = data;
    if (!st || !ui_lock()) {
        return;
    }

    paint_status(st);
    paint_box(st);

    /* The service ends enrolment on its own -- when the shots are in, or when
     * it times out -- so the screen notices rather than being told. */
    const bool finished = s_was_enrolling && !svc_face_enrolling();
    if (finished) {
        s_was_enrolling = false;
        build_list();
    }
    ui_unlock();

    if (finished) {
        ui_toast(svc_face_enroll_progress() >= FACE_SAMPLES_PER
                     ? "Face enrolled"
                     : "Enrolment gave up -- try again in better light");
    }
}

/* --------------------------------------------------------------------------
 * Build
 * ------------------------------------------------------------------------ */
static void create(lv_obj_t *parent)
{
    const lv_coord_t h      = ui_content_height() - 2 * UI_PAD;
    const lv_coord_t left_w = 216;
    const lv_coord_t right_w = ui_width() - 2 * UI_PAD - left_w - UI_PAD;

    lv_obj_t *cam = ui_card(parent, left_w, h);
    lv_obj_align(cam, LV_ALIGN_TOP_LEFT, 0, 0);

    s_view = ui_liveview_create(cam, left_w - 24, 150);
    lv_obj_align(ui_liveview_obj(s_view), LV_ALIGN_TOP_MID, 0, 4);
    ui_liveview_set_placeholder(s_view, "Camera off");

    s_status = ui_label(cam, "", &lv_font_montserrat_12, UI_COL_TEXT);
    lv_obj_align(s_status, LV_ALIGN_TOP_MID, 0, 160);

    s_bar = lv_bar_create(cam);
    lv_obj_set_size(s_bar, left_w - 32, 8);
    lv_obj_align(s_bar, LV_ALIGN_TOP_MID, 0, 182);
    lv_bar_set_range(s_bar, 0, FACE_SAMPLES_PER);
    lv_bar_set_value(s_bar, 0, LV_ANIM_OFF);

    s_enrol_btn = ui_button(cam, LV_SYMBOL_PLUS " Enrol a face",
                            enrol_clicked, NULL);
    lv_obj_set_size(s_enrol_btn, left_w - 28, 38);
    lv_obj_align(s_enrol_btn, LV_ALIGN_BOTTOM_MID, 0, -4);

    lv_obj_t *card = ui_card(parent, right_w, h);
    lv_obj_align(card, LV_ALIGN_TOP_RIGHT, 0, 0);
    ui_card_title(card, "Enrolled");

    s_list = lv_obj_create(card);
    lv_obj_remove_style_all(s_list);
    lv_obj_set_size(s_list, right_w - 24, h - 46);
    lv_obj_align(s_list, LV_ALIGN_BOTTOM_MID, 0, -8);
    ui_flex_col(s_list, 0);
    lv_obj_set_style_pad_all(s_list, 0, 0);

    s_pill = ui_pill(ui_header_slot(UI_SCREEN_FACES), "", UI_COL_MUTED);
}

static void on_enter(void)
{
    if (svc_face_ready()) {
        ui_pill_set(s_pill, "LBPH", UI_COL_PRIMARY);
    } else {
        ui_pill_set(s_pill, "Unavailable", UI_COL_DANGER);
    }

    build_list();
    paint_status(NULL);

    /* The live view is what makes enrolment aimable, so the camera runs for
     * as long as this page is open. */
    ui_liveview_attach_camera(s_view);
    app_event_subscribe(APP_EVT_FACE, on_face, NULL);
}

static void on_leave(void)
{
    app_event_unsubscribe(APP_EVT_FACE, on_face);
    ui_liveview_detach(s_view);

    /* Walking away mid-enrolment must not leave the camera enrolling the
     * next person who happens to stand in front of it. */
    if (svc_face_enrolling()) {
        svc_face_enroll_cancel();
    }
    s_was_enrolling = false;
}

const ui_screen_def_t ui_screen_faces_def = {
    .title    = "Faces",
    .create   = create,
    .on_enter = on_enter,
    .on_leave = on_leave,
};
