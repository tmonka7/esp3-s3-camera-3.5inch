/* The credential list: what opens the door, and what used to.
 *
 * Deliberately plain. Every row is one credential with a switch that decides
 * whether it works and a bin that removes it -- a lost card should be one tap
 * away from being useless, without deleting the history of where it went.
 */
#include <stdio.h>
#include <string.h>

#include "app_events.h"
#include "svc_access.h"
#include "svc_face.h"
#include "ui_internal.h"

static lv_obj_t *s_list;
static lv_obj_t *s_summary;
static size_t    s_editing;       /* index the rename dialog is working on */

static void build_list(void);

/* --------------------------------------------------------------------------
 * Row actions
 * ------------------------------------------------------------------------ */
static void enabled_changed(lv_event_t *e)
{
    const size_t    index = (size_t)(uintptr_t)lv_event_get_user_data(e);
    lv_obj_t       *sw    = lv_event_get_target(e);
    const bool      on    = lv_obj_has_state(sw, LV_STATE_CHECKED);

    if (svc_access_cred_set_enabled(index, on) != ESP_OK) {
        ui_toast("%s", UI_T(CRD_SAVE_FAIL));
        return;
    }
    ui_toast("%s", on ? UI_T(ENABLED) : UI_T(DISABLED));
}

/* --------------------------------------------------------------------------
 * Adding a credential
 * ------------------------------------------------------------------------ */
/**
 * Accepts "04A2B31C", "04 A2 B3 1C" and "04:A2:B3:1C" alike, because a UID
 * copied from another system arrives in all three forms. Everything that is
 * not a hex digit is skipped.
 */
static bool parse_uid(const char *s, uint8_t *out, uint8_t *out_len)
{
    uint8_t nibbles[BSP_RFID_UID_MAX * 2];
    size_t  n = 0;

    for (; *s; s++) {
        uint8_t v;
        if (*s >= '0' && *s <= '9') {
            v = (uint8_t)(*s - '0');
        } else if (*s >= 'a' && *s <= 'f') {
            v = (uint8_t)(*s - 'a' + 10);
        } else if (*s >= 'A' && *s <= 'F') {
            v = (uint8_t)(*s - 'A' + 10);
        } else {
            continue;
        }
        if (n >= sizeof(nibbles)) {
            return false;
        }
        nibbles[n++] = v;
    }

    /* A UID is 4, 7 or 10 bytes -- anything else is a typo, and guessing at
     * what was meant would silently enrol the wrong card. */
    if (n != 8 && n != 14 && n != 20) {
        return false;
    }

    for (size_t i = 0; i < n / 2; i++) {
        out[i] = (uint8_t)((nibbles[i * 2] << 4) | nibbles[i * 2 + 1]);
    }
    *out_len = (uint8_t)(n / 2);
    return true;
}

static void apply_manual_uid(const char *text)
{
    uint8_t uid[BSP_RFID_UID_MAX];
    uint8_t len = 0;

    if (!parse_uid(text, uid, &len)) {
        ui_toast("%s", UI_T(CRD_HEX_HINT));
        return;
    }

    switch (svc_access_cred_add_card(uid, len, NULL)) {
    case ESP_OK:                ui_toast("%s", UI_T(CRD_ADDED));      break;
    case ESP_ERR_INVALID_STATE: ui_toast("%s", UI_T(CRD_ALREADY));    break;
    case ESP_ERR_NO_MEM:        ui_toast("%s", UI_T(CRD_LIST_FULL));  break;
    default:                    ui_toast("%s", UI_T(CRD_ADD_FAIL));   break;
    }
}

static void manual_clicked(lv_event_t *e)
{
    (void)e;
    ui_edit_text(UI_T(CRD_UID_HEX), "", false, apply_manual_uid, build_list);
}

static void scan_clicked(lv_event_t *e)
{
    (void)e;
    if (svc_access_enroll_begin(30) == ESP_OK) {
        ui_toast("%s", UI_T(CRD_PRESENT_30));
    } else {
        ui_toast("%s", UI_T(CRD_NO_READER));
    }
}

static void faces_clicked(lv_event_t *e)
{
    (void)e;
    ui_show(UI_SCREEN_FACES);
}

static void apply_rename(const char *text)
{
    if (text && text[0]) {
        svc_access_cred_rename(s_editing, text);
    }
}

static void rename_clicked(lv_event_t *e)
{
    s_editing = (size_t)(uintptr_t)lv_event_get_user_data(e);

    access_cred_t c;
    if (svc_access_cred_get(s_editing, &c) != ESP_OK) {
        return;
    }
    ui_edit_text(UI_T(CRD_NAME_PROMPT), c.name, false, apply_rename, build_list);
}

static void confirm_delete_yes(lv_event_t *e)
{
    const size_t index = (size_t)(uintptr_t)lv_event_get_user_data(e);

    ui_dialog_close(NULL);
    if (svc_access_cred_remove(index) == ESP_OK) {
        ui_toast("%s", UI_T(CRD_REMOVED));
        build_list();
    } else {
        ui_toast("%s", UI_T(CRD_REMOVE_FAIL));
    }
}

static void delete_clicked(lv_event_t *e)
{
    const size_t index = (size_t)(uintptr_t)lv_event_get_user_data(e);

    access_cred_t c;
    if (svc_access_cred_get(index, &c) != ESP_OK) {
        return;
    }

    char title[64];
    snprintf(title, sizeof(title), UI_T(CRD_REMOVE_FMT), c.name);

    lv_obj_t *panel = ui_dialog_shell(title, 116);
    ui_label(panel, UI_T(CRD_REMOVE_WARN),
             UI_FONT_12, UI_COL_MUTED);

    lv_obj_t *row = lv_obj_create(panel);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), 34);
    ui_flex_row(row, UI_PAD);
    lv_obj_set_width(ui_button_soft(row, UI_T(CANCEL), ui_dialog_close, NULL), 110);

    lv_obj_t *del = ui_button(row, UI_T(REMOVE), confirm_delete_yes,
                              (void *)(uintptr_t)index);
    lv_obj_set_width(del, 110);
    lv_obj_set_style_bg_color(del, UI_COL_DANGER, 0);
}

/* --------------------------------------------------------------------------
 * List
 * ------------------------------------------------------------------------ */
static void add_row(size_t index, const access_cred_t *c)
{
    lv_obj_t *row = lv_obj_create(s_list);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), 46);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_color(row, UI_COL_TRACK, 0);
    lv_obj_set_style_border_width(row, 1, 0);

    lv_obj_t *name = ui_label(row, c->name, UI_FONT_14, UI_COL_TEXT);
    lv_obj_align(name, LV_ALIGN_TOP_LEFT, 4, 4);

    /* The UID doubles as the "which card is this" answer when two are named
     * the same, so it is always shown rather than hidden behind the row. */
    char detail[64];
    if (c->kind == ACCESS_CRED_FACE) {
        snprintf(detail, sizeof(detail), UI_T(CRD_FACE_FMT), (unsigned)c->face_id);
    } else {
        char hex[BSP_RFID_UID_MAX * 2 + 1] = { 0 };
        for (uint8_t i = 0; i < c->uid_len && i < BSP_RFID_UID_MAX; i++) {
            snprintf(&hex[i * 2], 3, "%02X", c->uid[i]);
        }
        snprintf(detail, sizeof(detail), UI_T(CRD_CARD_FMT), hex);
    }

    lv_obj_t *sub = ui_label(row, detail, UI_FONT_12, UI_COL_MUTED);
    lv_obj_align(sub, LV_ALIGN_TOP_LEFT, 4, 24);

    if (c->use_count) {
        char uses[32];
        snprintf(uses, sizeof(uses), UI_T(CRD_USES_FMT), (unsigned)c->use_count);
        lv_obj_t *n = ui_label(row, uses, UI_FONT_12, UI_COL_MUTED);
        lv_obj_align(n, LV_ALIGN_TOP_RIGHT, -150, 24);
    }

    lv_obj_t *edit = ui_button_soft(row, LV_SYMBOL_EDIT, rename_clicked,
                                    (void *)(uintptr_t)index);
    lv_obj_set_size(edit, 40, 32);
    lv_obj_align(edit, LV_ALIGN_RIGHT_MID, -104, 0);

    lv_obj_t *bin = ui_button_soft(row, LV_SYMBOL_TRASH, delete_clicked,
                                   (void *)(uintptr_t)index);
    lv_obj_set_size(bin, 40, 32);
    lv_obj_align(bin, LV_ALIGN_RIGHT_MID, -58, 0);
    lv_obj_set_style_text_color(bin, UI_COL_DANGER, 0);

    lv_obj_t *sw = lv_switch_create(row);
    lv_obj_set_size(sw, 46, 26);
    lv_obj_align(sw, LV_ALIGN_RIGHT_MID, -4, 0);
    if (c->enabled) {
        lv_obj_add_state(sw, LV_STATE_CHECKED);
    }
    lv_obj_add_event_cb(sw, enabled_changed, LV_EVENT_VALUE_CHANGED,
                        (void *)(uintptr_t)index);
}

static void build_list(void)
{
    if (!s_list) {
        return;
    }
    lv_obj_clean(s_list);

    const size_t n = svc_access_cred_count();

    /* The face state is spelled out rather than left blank, because an absent
     * feature and a broken one look identical when you are hunting for it. */
    char summary[64];
    snprintf(summary, sizeof(summary), UI_T(CRD_SLOTS_FMT),
             (unsigned)n, ACCESS_MAX_CREDENTIALS,
             svc_face_subject_count() ? UI_T(READY) : UI_T(CRD_NONE_ENROLLED));
    lv_label_set_text(s_summary, summary);

    if (n == 0) {
        lv_obj_t *empty = ui_label(s_list,
                                   UI_T(CRD_EMPTY),
                                   UI_FONT_12, UI_COL_MUTED);
        lv_obj_set_width(empty, LV_PCT(100));
        return;
    }

    for (size_t i = 0; i < n; i++) {
        access_cred_t c;
        if (svc_access_cred_get(i, &c) == ESP_OK) {
            add_row(i, &c);
        }
    }
}

/* --------------------------------------------------------------------------
 * Build
 * ------------------------------------------------------------------------ */
static void create(lv_obj_t *parent)
{
    const lv_coord_t w = ui_width() - 2 * UI_PAD;
    const lv_coord_t h = ui_content_height() - 2 * UI_PAD;

    lv_obj_t *card = ui_card(parent, w, h);
    lv_obj_align(card, LV_ALIGN_TOP_MID, 0, 0);

    ui_card_title(card, UI_T(SET_CREDENTIALS));

    s_summary = ui_label(card, "", UI_FONT_12, UI_COL_MUTED);
    lv_obj_align(s_summary, LV_ALIGN_TOP_RIGHT, -12, 12);

    /* Both ways of adding a card sit together: scanning is the normal one,
     * typing the UID is what gets you through commissioning before the
     * reader is wired -- or when it never will be. */
    char scan_lbl[40], manual_lbl[40], faces_lbl[40];
    snprintf(scan_lbl,   sizeof(scan_lbl),   LV_SYMBOL_REFRESH  "%s", UI_T(CRD_SCAN_CARD));
    snprintf(manual_lbl, sizeof(manual_lbl), LV_SYMBOL_KEYBOARD "%s", UI_T(CRD_ENTER_UID));
    snprintf(faces_lbl,  sizeof(faces_lbl),  LV_SYMBOL_EYE_OPEN "%s", UI_T(CRD_FACES_BTN));

    lv_obj_t *scan = ui_button(card, scan_lbl,
                               scan_clicked, NULL);
    lv_obj_set_size(scan, 140, 32);
    lv_obj_align(scan, LV_ALIGN_TOP_LEFT, 0, 30);

    lv_obj_t *manual = ui_button_soft(card, manual_lbl,
                                      manual_clicked, NULL);
    lv_obj_set_size(manual, 140, 32);
    lv_obj_align(manual, LV_ALIGN_TOP_LEFT, 148, 30);

    lv_obj_t *faces = ui_button_soft(card, faces_lbl,
                                     faces_clicked, NULL);
    lv_obj_set_size(faces, 140, 32);
    lv_obj_align(faces, LV_ALIGN_TOP_LEFT, 296, 30);

    s_list = lv_obj_create(card);
    lv_obj_remove_style_all(s_list);
    lv_obj_set_size(s_list, w - 24, h - 84);
    lv_obj_align(s_list, LV_ALIGN_BOTTOM_MID, 0, -8);
    ui_flex_col(s_list, 0);
    lv_obj_set_style_pad_all(s_list, 0, 0);
}

/** A card scanned while this page is open should appear without a round trip. */
static void on_access(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base; (void)id;

    const access_event_t *e = data;
    if (!e || e->result != ACCESS_ENROLLED || !ui_lock()) {
        return;
    }
    build_list();
    ui_unlock();

    ui_toast(UI_T(ACC_ENROLLED_FMT), e->name);
}

static void on_enter(void)
{
    build_list();
    app_event_subscribe(APP_EVT_ACCESS, on_access, NULL);
}

static void on_leave(void)
{
    app_event_unsubscribe(APP_EVT_ACCESS, on_access);

    /* A confirmation left open would reappear over whatever screen comes
     * next, still holding the index it was opened with. */
    ui_dialog_close(NULL);

    /* Same reasoning as the Door page: an enrolment window left open is how
     * a stranger's card gets added. */
    svc_access_enroll_cancel();
}

const ui_screen_def_t ui_screen_cards_def = {
    .title    = UI_STR_TITLE_CARDS,
    .create   = create,
    .on_enter = on_enter,
    .on_leave = on_leave,
};
