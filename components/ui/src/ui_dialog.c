/* The modal edit dialogs.
 *
 * Two shapes cover every edit in the firmware: a bounded number and a line of
 * text. Both are modal so a half-finished edit cannot be left behind on a
 * screen change.
 *
 * This started inside the Settings screen and moved out when the credential
 * list needed to rename a card. The one thing that had to change on the way
 * out is that the screen now says what to refresh when an edit is applied,
 * instead of the dialog knowing Settings by name.
 */
#include <string.h>

#include "ui_internal.h"

static lv_obj_t         *s_dialog;
static lv_obj_t         *s_dialog_field;
static ui_apply_number_t s_apply_number;
static ui_apply_text_t   s_apply_text;
static void            (*s_on_done)(void);

void ui_dialog_close(lv_event_t *e)
{
    (void)e;
    if (s_dialog) {
        lv_obj_del(s_dialog);
        s_dialog       = NULL;
        s_dialog_field = NULL;
        s_apply_number = NULL;
        s_apply_text   = NULL;
        s_on_done      = NULL;
    }
}

static void dialog_ok(lv_event_t *e)
{
    (void)e;
    if (!s_dialog_field) {
        return;
    }

    /* Copy what we need before the dialog is torn down: the apply callback
     * usually rebuilds the panel underneath us, and on_done certainly does. */
    void (*done)(void) = s_on_done;

    if (s_apply_number) {
        const int              value = (int)lv_spinbox_get_value(s_dialog_field);
        const ui_apply_number_t fn   = s_apply_number;
        ui_dialog_close(NULL);
        fn(value);
    } else if (s_apply_text) {
        char text[96];
        strlcpy(text, lv_textarea_get_text(s_dialog_field), sizeof(text));
        const ui_apply_text_t fn = s_apply_text;
        ui_dialog_close(NULL);
        fn(text);
    } else {
        ui_dialog_close(NULL);
    }

    if (done) {
        done();
    }
}

lv_obj_t *ui_dialog_shell(const char *title, lv_coord_t height)
{
    ui_dialog_close(NULL);

    s_dialog = lv_obj_create(lv_scr_act());
    lv_obj_remove_style_all(s_dialog);
    lv_obj_set_size(s_dialog, ui_width(), ui_height());
    lv_obj_set_style_bg_color(s_dialog, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_dialog, LV_OPA_60, 0);
    lv_obj_clear_flag(s_dialog, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *panel = ui_card(s_dialog, ui_width() - 60, height);
    lv_obj_align(panel, LV_ALIGN_TOP_MID, 0, 16);
    ui_flex_col(panel, 8);

    ui_label(panel, title, &lv_font_montserrat_14, UI_COL_TEXT);
    return panel;
}

/** Cancel / Save pair, the same on both dialogs. */
static void add_buttons(lv_obj_t *panel)
{
    lv_obj_t *row = lv_obj_create(panel);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), 34);
    ui_flex_row(row, UI_PAD);
    lv_obj_set_width(ui_button_soft(row, "Cancel", ui_dialog_close, NULL), 110);
    lv_obj_set_width(ui_button(row,      "Save",   dialog_ok,       NULL), 110);
}

void ui_edit_number(const char *title, int min, int max, int current,
                    ui_apply_number_t apply, void (*on_done)(void))
{
    lv_obj_t *panel = ui_dialog_shell(title, 132);
    s_apply_number  = apply;
    s_on_done       = on_done;

    s_dialog_field = lv_spinbox_create(panel);
    lv_spinbox_set_range(s_dialog_field, min, max);
    lv_spinbox_set_digit_format(s_dialog_field, 5, 0);
    lv_spinbox_set_value(s_dialog_field, current);
    lv_obj_set_size(s_dialog_field, LV_PCT(100), 34);

    add_buttons(panel);
}

void ui_edit_text(const char *title, const char *current, bool password,
                  ui_apply_text_t apply, void (*on_done)(void))
{
    lv_obj_t *panel = ui_dialog_shell(title, 118);
    s_apply_text    = apply;
    s_on_done       = on_done;

    s_dialog_field = lv_textarea_create(panel);
    lv_textarea_set_one_line(s_dialog_field, true);
    lv_textarea_set_password_mode(s_dialog_field, password);
    lv_textarea_set_text(s_dialog_field, current ? current : "");
    lv_obj_set_size(s_dialog_field, LV_PCT(100), 34);

    add_buttons(panel);

    /* Parented to the overlay rather than the card so it covers the bottom of
     * the screen instead of being clipped by the panel. */
    lv_obj_t *kb = lv_keyboard_create(s_dialog);
    lv_obj_set_size(kb, ui_width(), 150);
    lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_textarea(kb, s_dialog_field);
}
