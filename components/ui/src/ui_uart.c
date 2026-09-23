#include <stdio.h>
#include <string.h>

#include "app_events.h"
#include "app_settings.h"
#include "bsp_board.h"
#include "svc_uart.h"
#include "ui_internal.h"

#define PANEL_W       142
/* Enough scrollback to be useful without letting the textarea grow until it
 * starves LVGL's heap. Trimmed from the front when exceeded. */
#define RX_MAX_CHARS  3000

static const uint32_t k_bauds[] = { 9600, 19200, 38400, 57600, 115200, 230400, 460800 };
static const char *k_baud_opts   = "9600\n19200\n38400\n57600\n115200\n230400\n460800";
static const char *k_format_opts = "8N1\n8E1\n8O1\n7E1\n8N2";

static lv_obj_t *s_rx;
static lv_obj_t *s_baud_dd;
static lv_obj_t *s_format_dd;
static lv_obj_t *s_pill;
static lv_obj_t *s_open_btn;
static lv_obj_t *s_close_btn;
static size_t    s_rx_chars;

/* --------------------------------------------------------------------------
 * Port settings
 * ------------------------------------------------------------------------ */
static void apply_format(uint16_t sel, app_settings_t *cfg)
{
    /* Index matches k_format_opts. */
    static const uint8_t data[]  = { 8, 8, 8, 7, 8 };
    static const uint8_t parity[] = { 0, 2, 1, 2, 0 };
    static const uint8_t stop[]   = { 1, 1, 1, 1, 2 };

    if (sel >= sizeof(data) / sizeof(data[0])) {
        return;
    }
    cfg->uart_databits = data[sel];
    cfg->uart_parity   = parity[sel];
    cfg->uart_stopbits = stop[sel];
}

static uint16_t format_index(const app_settings_t *cfg)
{
    if (cfg->uart_databits == 7 && cfg->uart_parity == 2)  return 3;
    if (cfg->uart_stopbits == 2)                            return 4;
    if (cfg->uart_parity == 2)                              return 1;
    if (cfg->uart_parity == 1)                              return 2;
    return 0;
}

static void update_link_ui(bool open)
{
    ui_pill_set(s_pill, open ? UI_T(UART_CONNECTED) : UI_T(UART_CLOSED),
                open ? UI_COL_PRIMARY : UI_COL_MUTED);

    /* Only one of the two is ever the sensible action. */
    lv_obj_set_style_bg_color(s_open_btn,  open ? UI_COL_TRACK : UI_COL_PRIMARY, 0);
    lv_obj_set_style_bg_color(s_close_btn, open ? UI_COL_PRIMARY : UI_COL_TRACK, 0);
}

static void do_open(lv_event_t *e)
{
    (void)e;

    app_settings_t *cfg = app_settings();
    const uint16_t  bi  = lv_dropdown_get_selected(s_baud_dd);
    if (bi < sizeof(k_bauds) / sizeof(k_bauds[0])) {
        cfg->uart_baud = k_bauds[bi];
    }
    apply_format(lv_dropdown_get_selected(s_format_dd), cfg);
    app_settings_commit_deferred();

    if (svc_uart_open(NULL) == ESP_OK) {
        ui_toast(UI_T(UART_OPEN_FMT), BSP_UART_NUM, (unsigned long)cfg->uart_baud);
    } else {
        ui_toast("%s", UI_T(UART_OPEN_FAIL));
    }
}

static void do_close(lv_event_t *e)
{
    (void)e;
    svc_uart_close();
    ui_toast("%s", UI_T(UART_PORT_CLOSED));
}

/* --------------------------------------------------------------------------
 * Receive area
 * ------------------------------------------------------------------------ */
static void append_line(const svc_uart_line_t *line)
{
    char row[SVC_UART_LINE_MAX + 24];
    const int n = snprintf(row, sizeof(row), "[%s] %s%s\n", line->stamp,
                           line->is_tx ? "> " : "", line->text);
    if (n <= 0) {
        return;
    }

    /* Trim from the front once the buffer is long enough to matter. */
    if (s_rx_chars + (size_t)n > RX_MAX_CHARS) {
        lv_textarea_set_text(s_rx, "");
        s_rx_chars = 0;
    }

    lv_textarea_add_text(s_rx, row);
    s_rx_chars += (size_t)n;

    /* Follow the tail, which is what a serial console is for. */
    lv_textarea_set_cursor_pos(s_rx, LV_TEXTAREA_CURSOR_LAST);
}

static void on_rx(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base; (void)id;

    const svc_uart_line_t *line = data;
    if (!line || !ui_lock()) {
        return;
    }
    append_line(line);
    ui_unlock();
}

static void on_state(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base; (void)id;

    const app_link_state_t *st = data;
    if (!st || !ui_lock()) {
        return;
    }
    update_link_ui(*st == APP_LINK_UP);
    ui_unlock();
}

/* --------------------------------------------------------------------------
 * Toolbar
 * ------------------------------------------------------------------------ */
static void do_clear(lv_event_t *e)
{
    (void)e;
    lv_textarea_set_text(s_rx, "");
    s_rx_chars = 0;
    svc_uart_history_clear();
}

static void do_save(lv_event_t *e)
{
    (void)e;

    char path[96];
    if (svc_uart_save_log(path, sizeof(path)) == ESP_OK) {
        const char *name = strrchr(path, '/');
        ui_toast(UI_T(UART_SAVED_FMT), name ? name + 1 : path);
    } else {
        ui_toast("%s", UI_T(UART_SAVE_FAIL));
    }
}

/* ---- send dialog ---- */
static lv_obj_t *s_dialog;
static lv_obj_t *s_dialog_input;

static void dialog_close(lv_event_t *e)
{
    (void)e;
    if (s_dialog) {
        lv_obj_del(s_dialog);
        s_dialog       = NULL;
        s_dialog_input = NULL;
    }
}

static void dialog_send(lv_event_t *e)
{
    (void)e;
    if (!s_dialog_input) {
        return;
    }

    const char *text = lv_textarea_get_text(s_dialog_input);
    esp_err_t   err;

    /* Hex view implies hex entry: typing "01 03 00" in ASCII mode would send
     * the literal characters, which is never what the user means here. */
    if (app_settings()->uart_hex_view) {
        err = svc_uart_send_hex(text);
    } else {
        err = svc_uart_send_line(text);
    }

    if (err != ESP_OK) {
        ui_toast("%s", UI_T(UART_SEND_FAIL));
    }
    dialog_close(NULL);
}

static void do_send(lv_event_t *e)
{
    (void)e;

    if (!svc_uart_is_open()) {
        ui_toast("%s", UI_T(UART_OPEN_FIRST));
        return;
    }

    s_dialog = lv_obj_create(lv_scr_act());
    lv_obj_remove_style_all(s_dialog);
    lv_obj_set_size(s_dialog, ui_width(), ui_height());
    lv_obj_set_style_bg_color(s_dialog, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_dialog, LV_OPA_60, 0);
    lv_obj_clear_flag(s_dialog, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *panel = ui_card(s_dialog, ui_width() - 40, 120);
    lv_obj_align(panel, LV_ALIGN_TOP_MID, 0, 14);

    ui_label(panel, app_settings()->uart_hex_view ? UI_T(UART_SEND_HEX) : UI_T(UART_SEND_TEXT),
             UI_FONT_14, UI_COL_TEXT);

    s_dialog_input = lv_textarea_create(panel);
    lv_obj_set_size(s_dialog_input, LV_PCT(100), 42);
    lv_obj_align(s_dialog_input, LV_ALIGN_TOP_MID, 0, 22);
    lv_textarea_set_one_line(s_dialog_input, true);
    lv_textarea_set_placeholder_text(s_dialog_input,
                                     app_settings()->uart_hex_view ? "01 03 00 00"
                                                                   : "AT");

    lv_obj_t *send   = ui_button(panel, UI_T(UART_SEND), dialog_send, NULL);
    lv_obj_align(send, LV_ALIGN_BOTTOM_RIGHT, 0, 0);

    lv_obj_t *cancel = ui_button_soft(panel, UI_T(CANCEL), dialog_close, NULL);
    lv_obj_align(cancel, LV_ALIGN_BOTTOM_RIGHT, -78, 0);

    lv_obj_t *kb = lv_keyboard_create(s_dialog);
    lv_obj_set_size(kb, ui_width(), 150);
    lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_mode(kb, app_settings()->uart_hex_view ? LV_KEYBOARD_MODE_NUMBER
                                                           : LV_KEYBOARD_MODE_TEXT_LOWER);
    lv_keyboard_set_textarea(kb, s_dialog_input);
}

/* --------------------------------------------------------------------------
 * Layout
 * ------------------------------------------------------------------------ */
static void create(lv_obj_t *parent)
{
    const lv_coord_t h = ui_content_height() - 2 * UI_PAD;

    /* ---- left: port settings ---- */
    lv_obj_t *cfg_card = ui_card(parent, PANEL_W, h);
    lv_obj_align(cfg_card, LV_ALIGN_TOP_LEFT, 0, 0);
    ui_flex_col(cfg_card, 6);

    ui_card_title(cfg_card, UI_T(UART_PORT_SET));

    char port_text[24];
    snprintf(port_text, sizeof(port_text), "UART%d  (TX %d / RX %d)",
             BSP_UART_NUM, BSP_UART_PIN_TX, BSP_UART_PIN_RX);
    ui_label(cfg_card, port_text, UI_FONT_12, UI_COL_MUTED);

    s_baud_dd = lv_dropdown_create(cfg_card);
    lv_dropdown_set_options_static(s_baud_dd, k_baud_opts);
    lv_obj_set_width(s_baud_dd, LV_PCT(100));

    s_format_dd = lv_dropdown_create(cfg_card);
    lv_dropdown_set_options_static(s_format_dd, k_format_opts);
    lv_obj_set_width(s_format_dd, LV_PCT(100));

    const app_settings_t *cfg = app_settings();
    for (size_t i = 0; i < sizeof(k_bauds) / sizeof(k_bauds[0]); i++) {
        if (k_bauds[i] == cfg->uart_baud) {
            lv_dropdown_set_selected(s_baud_dd, (uint16_t)i);
            break;
        }
    }
    lv_dropdown_set_selected(s_format_dd, format_index(cfg));

    s_open_btn = ui_button(cfg_card, UI_T(OPEN), do_open, NULL);
    lv_obj_set_width(s_open_btn, LV_PCT(100));

    s_close_btn = ui_button(cfg_card, UI_T(CLOSE), do_close, NULL);
    lv_obj_set_width(s_close_btn, LV_PCT(100));

    /* ---- right: receive area ---- */
    const lv_coord_t rx_w = ui_width() - PANEL_W - 3 * UI_PAD;
    const lv_coord_t bar_h = 34;

    lv_obj_t *rx_card = ui_card(parent, rx_w, h - bar_h - 6);
    lv_obj_align(rx_card, LV_ALIGN_TOP_RIGHT, 0, 0);
    lv_obj_set_style_pad_all(rx_card, 6, 0);

    ui_card_title(rx_card, UI_T(UART_RECEIVE));

    s_rx = lv_textarea_create(rx_card);
    lv_obj_set_size(s_rx, LV_PCT(100), h - bar_h - 6 - 34);
    lv_obj_align(s_rx, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_textarea_set_text(s_rx, "");
    lv_obj_set_style_text_font(s_rx, UI_FONT_12, 0);
    lv_obj_set_style_border_width(s_rx, 0, 0);
    lv_obj_set_style_bg_color(s_rx, lv_color_hex(0xF7F9F8), 0);
    /* Read-only: this is a log, and a caret here would invite typing into it. */
    lv_obj_clear_flag(s_rx, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_textarea_set_cursor_click_pos(s_rx, false);

    lv_obj_t *bar = lv_obj_create(parent);
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, rx_w, bar_h);
    lv_obj_align(bar, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    ui_flex_row(bar, UI_PAD);

    /* The glyph and the caption are separate strings now, so they are
     * joined here rather than concatenated by the preprocessor. */
    char clear_lbl[40], save_lbl[40], send_lbl[40];
    snprintf(clear_lbl, sizeof(clear_lbl), LV_SYMBOL_TRASH  "%s", UI_T(UART_CLEAR_BTN));
    snprintf(save_lbl,  sizeof(save_lbl),  LV_SYMBOL_SAVE   "%s", UI_T(UART_SAVE_BTN));
    snprintf(send_lbl,  sizeof(send_lbl),  LV_SYMBOL_UPLOAD "%s", UI_T(UART_SEND_BTN));

    const lv_coord_t bw = (rx_w - 2 * UI_PAD) / 3;
    lv_obj_set_width(ui_button_soft(bar, clear_lbl, do_clear, NULL), bw);
    lv_obj_set_width(ui_button_soft(bar, save_lbl,  do_save,  NULL), bw);
    lv_obj_set_width(ui_button(bar,      send_lbl,  do_send,  NULL), bw);

    s_pill = ui_pill(ui_header_slot(UI_SCREEN_UART), UI_T(UART_CLOSED), UI_COL_MUTED);
}

static void on_enter(void)
{
    update_link_ui(svc_uart_is_open());

    /* Replay the scrollback so the view is not empty after a screen change. */
    lv_textarea_set_text(s_rx, "");
    s_rx_chars = 0;

    const size_t n = svc_uart_history_count();
    const size_t first = (n > 40) ? n - 40 : 0;
    for (size_t i = first; i < n; i++) {
        svc_uart_line_t line;
        if (svc_uart_history_get(i, &line)) {
            append_line(&line);
        }
    }

    app_event_subscribe(APP_EVT_UART_RX, on_rx, NULL);
    app_event_subscribe(APP_EVT_UART_STATE, on_state, NULL);
}

static void on_leave(void)
{
    app_event_unsubscribe(APP_EVT_UART_RX, on_rx);
    app_event_unsubscribe(APP_EVT_UART_STATE, on_state);
    dialog_close(NULL);
}

const ui_screen_def_t ui_screen_uart_def = {
    .title    = UI_STR_TITLE_UART,
    .create   = create,
    .on_enter = on_enter,
    .on_leave = on_leave,
};
