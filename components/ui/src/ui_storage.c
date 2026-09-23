#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app_events.h"
#include "bsp_storage.h"
#include "media_files.h"
#include "ui_internal.h"

#define TAB_W        104
#define VISIBLE_ROWS 5

typedef enum {
    TAB_FILES = 0,
    TAB_VIDEOS,
    TAB_IMAGES,
} storage_tab_t;

static lv_obj_t      *s_tabs[3];
static lv_obj_t      *s_rows[VISIBLE_ROWS];
static lv_obj_t      *s_pill;
static lv_obj_t      *s_bar;
static lv_obj_t      *s_usage;
static lv_obj_t      *s_empty;
static storage_tab_t  s_tab;
static int            s_scroll;

/* Listings are rebuilt on demand rather than cached: a directory scan of a
 * few hundred entries is fast, and a stale list after a recording would be
 * worse than the scan. */
static media_entry_t *s_entries;
static size_t         s_entry_count;
static media_day_t    s_days[VISIBLE_ROWS * 4];
static size_t         s_day_count;

static media_kind_t kind_for_tab(void)
{
    switch (s_tab) {
    case TAB_VIDEOS: return MEDIA_KIND_VIDEO;
    case TAB_IMAGES: return MEDIA_KIND_IMAGE;
    default:         return MEDIA_KIND_ANY;
    }
}

static void row_clicked(lv_event_t *e);
static void refresh(void);

/* --------------------------------------------------------------------------
 * Capacity strip
 * ------------------------------------------------------------------------ */
static void refresh_capacity(void)
{
    bsp_storage_info_t info;
    if (bsp_storage_info(&info) != ESP_OK) {
        ui_pill_set(s_pill, UI_T(STO_NO_CARD), UI_COL_DANGER);
        lv_label_set_text(s_usage, UI_T(STO_NO_TF_CARD));
        lv_bar_set_value(s_bar, 0, LV_ANIM_OFF);
        return;
    }

    ui_pill_set(s_pill, UI_T(STO_TF_CARD), UI_COL_PRIMARY);

    const unsigned used_gb  = (unsigned)(info.used_bytes  >> 30);
    const unsigned used_mb  = (unsigned)((info.used_bytes  >> 20) % 1024);
    const unsigned total_gb = (unsigned)(info.total_bytes >> 30);

    char buf[48];
    snprintf(buf, sizeof(buf), UI_T(STO_USED_FMT),
             used_gb, (used_mb * 10) / 1024, total_gb);
    lv_label_set_text(s_usage, buf);

    const int pct = info.total_bytes
                  ? (int)((info.used_bytes * 100) / info.total_bytes) : 0;
    lv_bar_set_value(s_bar, pct, LV_ANIM_ON);
}

/* --------------------------------------------------------------------------
 * Day list
 * ------------------------------------------------------------------------ */
static void set_row(int i, const media_day_t *day)
{
    lv_obj_t *row = s_rows[i];
    if (!row) {
        return;
    }

    if (!day) {
        lv_obj_add_flag(row, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    lv_obj_clear_flag(row, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *icon    = lv_obj_get_child(row, 0);
    lv_obj_t *title   = lv_obj_get_child(row, 1);
    lv_obj_t *sub     = lv_obj_get_child(row, 2);

    const bool video = (day->kind == MEDIA_KIND_VIDEO);
    lv_label_set_text(icon, video ? LV_SYMBOL_VIDEO : LV_SYMBOL_IMAGE);
    lv_obj_set_style_text_color(icon, video ? UI_COL_PRIMARY : UI_COL_INFO, 0);

    lv_label_set_text(title, day->date);

    char size[24];
    media_files_format_size((uint32_t)day->bytes, size, sizeof(size));

    char detail[48];
    snprintf(detail, sizeof(detail), UI_T(STO_DAY_FMT),
             video ? UI_T(STO_VIDEO) : UI_T(STO_IMAGE), day->count, size);
    lv_label_set_text(sub, detail);
}

static void refresh(void)
{
    if (!s_entries) {
        return;
    }

    s_entry_count = media_files_list(kind_for_tab(), s_entries, MEDIA_MAX_ENTRIES);
    s_day_count   = media_files_group_by_day(s_entries, s_entry_count,
                                             s_days, sizeof(s_days) / sizeof(s_days[0]));

    if (s_scroll > (int)s_day_count - VISIBLE_ROWS) {
        s_scroll = (int)s_day_count - VISIBLE_ROWS;
    }
    if (s_scroll < 0) {
        s_scroll = 0;
    }

    for (int i = 0; i < VISIBLE_ROWS; i++) {
        const size_t idx = (size_t)(s_scroll + i);
        set_row(i, idx < s_day_count ? &s_days[idx] : NULL);
    }

    if (s_empty) {
        if (s_day_count == 0) {
            lv_obj_clear_flag(s_empty, LV_OBJ_FLAG_HIDDEN);
            lv_label_set_text(s_empty, bsp_storage_mounted()
                                       ? UI_T(STO_NOTHING)
                                       : UI_T(STO_INSERT));
        } else {
            lv_obj_add_flag(s_empty, LV_OBJ_FLAG_HIDDEN);
        }
    }

    refresh_capacity();
}

static void row_clicked(lv_event_t *e)
{
    const int i = (int)(intptr_t)lv_event_get_user_data(e);
    const size_t idx = (size_t)(s_scroll + i);
    if (idx >= s_day_count) {
        return;
    }

    /* Video days open in the player; image days have nothing to play, so
     * they just report what is there. */
    if (s_days[idx].kind == MEDIA_KIND_VIDEO) {
        ui_show(UI_SCREEN_PLAYBACK);
    } else {
        ui_toast(UI_T(STO_IMG_COUNT_FMT), s_days[idx].date, s_days[idx].count);
    }
}

static void scroll_clicked(lv_event_t *e)
{
    s_scroll += (int)(intptr_t)lv_event_get_user_data(e);
    refresh();
}

static void tab_clicked(lv_event_t *e)
{
    s_tab    = (storage_tab_t)(intptr_t)lv_event_get_user_data(e);
    s_scroll = 0;

    for (int i = 0; i < 3; i++) {
        const bool on = (i == (int)s_tab);
        lv_obj_set_style_bg_color(s_tabs[i], on ? UI_COL_PRIMARY : UI_COL_CARD, 0);
        lv_obj_set_style_text_color(lv_obj_get_child(s_tabs[i], 0),
                                    on ? lv_color_white() : UI_COL_MUTED, 0);
    }
    refresh();
}

/* --------------------------------------------------------------------------
 * Layout
 * ------------------------------------------------------------------------ */
static void create(lv_obj_t *parent)
{
    const lv_coord_t w = ui_width() - 2 * UI_PAD;
    const lv_coord_t h = ui_content_height() - 2 * UI_PAD;

    /* ---- tabs ---- */
    lv_obj_t *tabs = ui_card(parent, TAB_W, h);
    lv_obj_align(tabs, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_pad_all(tabs, 5, 0);
    ui_flex_col(tabs, 4);

    const ui_str_t k_tab_names[3] = {
        UI_STR_STO_FILES, UI_STR_STO_VIDEOS, UI_STR_STO_IMAGES,
    };
    static const char *k_tab_icons[3] = {
        LV_SYMBOL_DIRECTORY, LV_SYMBOL_VIDEO, LV_SYMBOL_IMAGE,
    };

    for (int i = 0; i < 3; i++) {
        lv_obj_t *tab = lv_btn_create(tabs);
        lv_obj_remove_style_all(tab);
        lv_obj_set_size(tab, LV_PCT(100), 32);
        lv_obj_set_style_radius(tab, 6, 0);
        lv_obj_set_style_bg_color(tab, i == 0 ? UI_COL_PRIMARY : UI_COL_CARD, 0);
        lv_obj_set_style_bg_opa(tab, LV_OPA_COVER, 0);
        lv_obj_clear_flag(tab, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *lbl = ui_label(tab, ui_tr(k_tab_names[i]), UI_FONT_12,
                                 i == 0 ? lv_color_white() : UI_COL_MUTED);
        lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 24, 0);

        lv_obj_t *icon = ui_label(tab, k_tab_icons[i], UI_FONT_12,
                                  i == 0 ? lv_color_white() : UI_COL_MUTED);
        lv_obj_align(icon, LV_ALIGN_LEFT_MID, 5, 0);

        lv_obj_add_event_cb(tab, tab_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        s_tabs[i] = tab;
    }

    /* ---- list ---- */
    const lv_coord_t list_w  = w - TAB_W - UI_PAD;
    const lv_coord_t strip_h = 30;

    lv_obj_t *list = ui_card(parent, list_w, h - strip_h - UI_PAD);
    lv_obj_align(list, LV_ALIGN_TOP_RIGHT, 0, 0);
    lv_obj_set_style_pad_all(list, 5, 0);
    ui_flex_col(list, 3);

    for (int i = 0; i < VISIBLE_ROWS; i++) {
        lv_obj_t *row = lv_btn_create(list);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, LV_PCT(100), 36);
        lv_obj_set_style_bg_color(row, lv_color_hex(0xF7F9F8), 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(row, 6, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(row, LV_OBJ_FLAG_HIDDEN);

        lv_obj_t *icon = ui_label(row, LV_SYMBOL_VIDEO, UI_FONT_16,
                                  UI_COL_PRIMARY);
        lv_obj_align(icon, LV_ALIGN_LEFT_MID, 6, 0);

        lv_obj_t *title = ui_label(row, "", UI_FONT_12, UI_COL_TEXT);
        lv_obj_align(title, LV_ALIGN_LEFT_MID, 32, -7);

        lv_obj_t *sub = ui_label(row, "", UI_FONT_12, UI_COL_MUTED);
        lv_obj_align(sub, LV_ALIGN_LEFT_MID, 32, 8);

        lv_obj_t *chev = ui_label(row, LV_SYMBOL_RIGHT, UI_FONT_12,
                                  UI_COL_MUTED);
        lv_obj_align(chev, LV_ALIGN_RIGHT_MID, -6, 0);

        lv_obj_add_event_cb(row, row_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        s_rows[i] = row;
    }

    s_empty = ui_label(list, UI_T(STO_INSERT), UI_FONT_14, UI_COL_MUTED);
    lv_obj_add_flag(s_empty, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_center(s_empty);

    /* ---- capacity strip ---- */
    lv_obj_t *strip = ui_card(parent, list_w, strip_h);
    lv_obj_align(strip, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_obj_set_style_pad_all(strip, 4, 0);

    s_usage = ui_label(strip, "--", UI_FONT_12, UI_COL_MUTED);
    lv_obj_align(s_usage, LV_ALIGN_LEFT_MID, 2, 0);

    s_bar = lv_bar_create(strip);
    lv_obj_set_size(s_bar, 110, 6);
    lv_obj_align(s_bar, LV_ALIGN_RIGHT_MID, -52, 0);
    lv_bar_set_range(s_bar, 0, 100);
    lv_obj_set_style_bg_color(s_bar, UI_COL_TRACK, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_bar, UI_COL_PRIMARY, LV_PART_INDICATOR);

    lv_obj_t *up = ui_button_soft(strip, LV_SYMBOL_UP, scroll_clicked,
                                  (void *)(intptr_t)-VISIBLE_ROWS);
    lv_obj_set_size(up, 22, 22);
    lv_obj_align(up, LV_ALIGN_RIGHT_MID, -24, 0);

    lv_obj_t *down = ui_button_soft(strip, LV_SYMBOL_DOWN, scroll_clicked,
                                    (void *)(intptr_t)VISIBLE_ROWS);
    lv_obj_set_size(down, 22, 22);
    lv_obj_align(down, LV_ALIGN_RIGHT_MID, 0, 0);

    s_pill = ui_pill(ui_header_slot(UI_SCREEN_STORAGE), UI_T(STO_TF_CARD), UI_COL_MUTED);
}

static void on_enter(void)
{
    if (!s_entries) {
        /* One allocation reused for every listing; freed when the screen is
         * left so a rarely-visited page does not hold 20 KB permanently. */
        s_entries = calloc(MEDIA_MAX_ENTRIES, sizeof(media_entry_t));
    }
    refresh();
}

static void on_leave(void)
{
    free(s_entries);
    s_entries     = NULL;
    s_entry_count = 0;
    s_day_count   = 0;
}

const ui_screen_def_t ui_screen_storage_def = {
    .title    = UI_STR_TITLE_STORAGE,
    .create   = create,
    .on_enter = on_enter,
    .on_leave = on_leave,
};
