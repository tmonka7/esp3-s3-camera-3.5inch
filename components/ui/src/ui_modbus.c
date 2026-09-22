#include <stdio.h>
#include <string.h>

#include "app_events.h"
#include "app_settings.h"
#include "bsp_board.h"
#include "svc_modbus.h"
#include "ui_internal.h"

#define PANEL_W  132

/* Matches the order of k_function_opts. */
static const svc_modbus_fn_t k_functions[] = {
    MB_FN_READ_HOLDING,
    MB_FN_READ_INPUT,
    MB_FN_READ_COILS,
    MB_FN_READ_DISCRETE,
};
static const char *k_function_opts =
    "Read Holding Registers\n"
    "Read Input Registers\n"
    "Read Coils\n"
    "Read Discrete Inputs";

static const uint16_t k_quantities[] = { 1, 2, 4, 8, 10, 16, 32 };
static const char *k_quantity_opts = "1\n2\n4\n8\n10\n16\n32";

static lv_obj_t *s_pill;
static lv_obj_t *s_addr;
static lv_obj_t *s_fn_dd;
static lv_obj_t *s_qty_dd;
static lv_obj_t *s_data;
static lv_obj_t *s_dev_items[4];
static uint8_t   s_slave = 1;

/* --------------------------------------------------------------------------
 * Device list
 * ------------------------------------------------------------------------ */
typedef enum {
    DEV_RTU = 0,
    DEV_TCP,
    DEV_SLAVE_1,
    DEV_SLAVE_2,
} device_row_t;

static void refresh_devices(void)
{
    const svc_modbus_transport_t t = svc_modbus_get_transport();

    for (int i = 0; i < 4; i++) {
        if (!s_dev_items[i]) {
            continue;
        }

        bool active = false;
        switch (i) {
        case DEV_RTU:     active = (t == MB_TRANSPORT_RTU); break;
        case DEV_TCP:     active = (t == MB_TRANSPORT_TCP); break;
        case DEV_SLAVE_1: active = (s_slave == 1);          break;
        case DEV_SLAVE_2: active = (s_slave == 2);          break;
        default: break;
        }

        lv_obj_set_style_bg_color(s_dev_items[i],
                                  active ? UI_COL_PRIMARY_SOFT : UI_COL_CARD, 0);
        lv_obj_set_style_bg_opa(s_dev_items[i], LV_OPA_COVER, 0);

        lv_obj_t *dot = lv_obj_get_child(s_dev_items[i], 0);
        lv_obj_set_style_text_color(dot, active ? UI_COL_PRIMARY : UI_COL_TRACK, 0);
    }
}

static void device_clicked(lv_event_t *e)
{
    const device_row_t row = (device_row_t)(uintptr_t)lv_event_get_user_data(e);

    switch (row) {
    case DEV_RTU:
        if (svc_modbus_set_transport(MB_TRANSPORT_RTU) == ESP_OK) {
            ui_toast("Modbus-RTU active");
        } else {
            ui_toast("RTU could not be started");
        }
        break;

    case DEV_TCP:
        if (svc_modbus_set_transport(MB_TRANSPORT_TCP) == ESP_OK) {
            ui_toast("Modbus-TCP active");
        } else {
            ui_toast("TCP needs Wi-Fi and a reachable host");
        }
        break;

    case DEV_SLAVE_1: s_slave = 1; break;
    case DEV_SLAVE_2: s_slave = 2; break;
    }

    refresh_devices();
}

/* --------------------------------------------------------------------------
 * Transactions
 * ------------------------------------------------------------------------ */
static void show_result(const svc_modbus_result_t *r)
{
    if (!r->ok) {
        char msg[96];
        snprintf(msg, sizeof(msg), "Slave %u, function %u: %s\n",
                 r->slave, r->function, esp_err_to_name((esp_err_t)r->err));
        lv_textarea_set_text(s_data, msg);
        return;
    }

    /* Rendered as "<address>: 0xHHHH (dec)" -- installers read hex, the
     * decimal saves them converting scaled values by hand. */
    char body[512];
    size_t pos = 0;
    for (uint16_t i = 0; i < r->count && pos + 32 < sizeof(body); i++) {
        pos += (size_t)snprintf(body + pos, sizeof(body) - pos,
                                "%5u : 0x%04X (%u)\n",
                                (unsigned)(r->address + i),
                                r->values[i], r->values[i]);
    }
    body[pos] = '\0';
    lv_textarea_set_text(s_data, body);
}

static void on_result(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base; (void)id;

    const svc_modbus_result_t *r = data;
    if (!r || !ui_lock()) {
        return;
    }
    show_result(r);
    ui_unlock();
}

static void on_state(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base; (void)id;

    const app_link_state_t *st = data;
    if (!st || !ui_lock()) {
        return;
    }

    switch (*st) {
    case APP_LINK_UP:         ui_pill_set(s_pill, "Connected",  UI_COL_PRIMARY); break;
    case APP_LINK_CONNECTING: ui_pill_set(s_pill, "Connecting", UI_COL_WARN);    break;
    case APP_LINK_ERROR:      ui_pill_set(s_pill, "Error",      UI_COL_DANGER);  break;
    default:                  ui_pill_set(s_pill, "Offline",    UI_COL_MUTED);   break;
    }
    refresh_devices();

    ui_unlock();
}

static void do_read(lv_event_t *e)
{
    (void)e;

    const uint16_t addr = (uint16_t)lv_spinbox_get_value(s_addr);
    const uint16_t fi   = lv_dropdown_get_selected(s_fn_dd);
    const uint16_t qi   = lv_dropdown_get_selected(s_qty_dd);

    if (fi >= sizeof(k_functions) / sizeof(k_functions[0]) ||
        qi >= sizeof(k_quantities) / sizeof(k_quantities[0])) {
        return;
    }

    if (svc_modbus_ui_request(s_slave, k_functions[fi], addr,
                              k_quantities[qi], NULL) != ESP_OK) {
        /* The failure detail arrives via APP_EVT_MODBUS_RESULT; this is just
         * the immediate "nothing happened" feedback. */
        ui_toast("Read failed");
    }
}

/* ---- write dialog ---- */
static lv_obj_t *s_wdialog;
static lv_obj_t *s_wvalue;

static void wdialog_close(lv_event_t *e)
{
    (void)e;
    if (s_wdialog) {
        lv_obj_del(s_wdialog);
        s_wdialog = NULL;
        s_wvalue  = NULL;
    }
}

static void wdialog_commit(lv_event_t *e)
{
    (void)e;
    if (!s_wvalue) {
        return;
    }

    const uint16_t addr  = (uint16_t)lv_spinbox_get_value(s_addr);
    const uint16_t value = (uint16_t)lv_spinbox_get_value(s_wvalue);

    if (svc_modbus_ui_request(s_slave, MB_FN_WRITE_SINGLE_REG, addr, 1, &value) == ESP_OK) {
        ui_toast("Wrote %u to %u", value, addr);
    } else {
        ui_toast("Write failed");
    }
    wdialog_close(NULL);
}

static void do_write(lv_event_t *e)
{
    (void)e;

    s_wdialog = lv_obj_create(lv_scr_act());
    lv_obj_remove_style_all(s_wdialog);
    lv_obj_set_size(s_wdialog, ui_width(), ui_height());
    lv_obj_set_style_bg_color(s_wdialog, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_wdialog, LV_OPA_60, 0);
    lv_obj_clear_flag(s_wdialog, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *panel = ui_card(s_wdialog, 240, 150);
    lv_obj_center(panel);
    ui_flex_col(panel, 8);

    char hdr[64];
    snprintf(hdr, sizeof(hdr), "Write register %u on slave %u",
             (unsigned)lv_spinbox_get_value(s_addr), s_slave);
    ui_label(panel, hdr, &lv_font_montserrat_12, UI_COL_MUTED);

    s_wvalue = lv_spinbox_create(panel);
    lv_spinbox_set_range(s_wvalue, 0, 65535);
    lv_spinbox_set_digit_format(s_wvalue, 5, 0);
    lv_obj_set_width(s_wvalue, LV_PCT(100));

    lv_obj_t *row = lv_obj_create(panel);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), 36);
    ui_flex_row(row, UI_PAD);

    lv_obj_set_width(ui_button_soft(row, "Cancel", wdialog_close,  NULL), 100);
    lv_obj_set_width(ui_button(row,      "Write",  wdialog_commit, NULL), 100);
}

/* --------------------------------------------------------------------------
 * Layout
 * ------------------------------------------------------------------------ */
static lv_obj_t *add_device_row(lv_obj_t *parent, const char *title,
                                const char *subtitle, device_row_t row)
{
    lv_obj_t *item = lv_btn_create(parent);
    lv_obj_remove_style_all(item);
    lv_obj_set_size(item, LV_PCT(100), 32);
    lv_obj_set_style_radius(item, 6, 0);
    lv_obj_clear_flag(item, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *dot = ui_label(item, LV_SYMBOL_OK, &lv_font_montserrat_12, UI_COL_TRACK);
    lv_obj_align(dot, LV_ALIGN_LEFT_MID, 3, 0);

    lv_obj_t *name = ui_label(item, title, &lv_font_montserrat_12, UI_COL_TEXT);
    lv_obj_align(name, LV_ALIGN_LEFT_MID, 19, -6);

    lv_obj_t *sub = ui_label(item, subtitle, &lv_font_montserrat_12, UI_COL_MUTED);
    lv_obj_align(sub, LV_ALIGN_LEFT_MID, 19, 7);

    lv_obj_add_event_cb(item, device_clicked, LV_EVENT_CLICKED, (void *)(uintptr_t)row);
    s_dev_items[row] = item;
    return item;
}

static void create(lv_obj_t *parent)
{
    const lv_coord_t h = ui_content_height() - 2 * UI_PAD;

    /* ---- left: devices ---- */
    lv_obj_t *dev_card = ui_card(parent, PANEL_W, h);
    lv_obj_align(dev_card, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_pad_all(dev_card, 5, 0);
    ui_flex_col(dev_card, 3);

    ui_label(dev_card, "Device List", &lv_font_montserrat_12, UI_COL_MUTED);

    const app_settings_t *cfg = app_settings();

    char rtu_sub[32];
    snprintf(rtu_sub, sizeof(rtu_sub), "UART%d  %lu",
             BSP_RS485_UART_NUM, (unsigned long)cfg->mb_rtu_baud);
    add_device_row(dev_card, "Modbus-RTU", rtu_sub, DEV_RTU);

    add_device_row(dev_card, "Modbus-TCP", cfg->mb_tcp_host, DEV_TCP);
    add_device_row(dev_card, "Inverter", "ID: 1", DEV_SLAVE_1);
    add_device_row(dev_card, "Meter",    "ID: 2", DEV_SLAVE_2);

    /* ---- right: register access ---- */
    const lv_coord_t w = ui_width() - PANEL_W - 3 * UI_PAD;

    lv_obj_t *card = ui_card(parent, w, h);
    lv_obj_align(card, LV_ALIGN_TOP_RIGHT, 0, 0);
    lv_obj_set_style_pad_all(card, 6, 0);

    ui_card_title(card, "Register Read / Write");

    const lv_coord_t label_w = 62;
    const lv_coord_t field_x = label_w + 4;
    const lv_coord_t field_w = w - field_x - 12;

    lv_obj_t *l1 = ui_label(card, "Address", &lv_font_montserrat_12, UI_COL_MUTED);
    lv_obj_set_pos(l1, 0, 26);

    s_addr = lv_spinbox_create(card);
    lv_spinbox_set_range(s_addr, 0, 65535);
    lv_spinbox_set_digit_format(s_addr, 5, 0);
    lv_obj_set_size(s_addr, field_w, 26);
    lv_obj_set_pos(s_addr, field_x, 22);

    lv_obj_t *l2 = ui_label(card, "Function", &lv_font_montserrat_12, UI_COL_MUTED);
    lv_obj_set_pos(l2, 0, 56);

    s_fn_dd = lv_dropdown_create(card);
    lv_dropdown_set_options_static(s_fn_dd, k_function_opts);
    lv_obj_set_size(s_fn_dd, field_w, 26);
    lv_obj_set_pos(s_fn_dd, field_x, 52);

    lv_obj_t *l3 = ui_label(card, "Quantity", &lv_font_montserrat_12, UI_COL_MUTED);
    lv_obj_set_pos(l3, 0, 86);

    s_qty_dd = lv_dropdown_create(card);
    lv_dropdown_set_options_static(s_qty_dd, k_quantity_opts);
    lv_obj_set_size(s_qty_dd, field_w, 26);
    lv_obj_set_pos(s_qty_dd, field_x, 82);
    lv_dropdown_set_selected(s_qty_dd, 4);      /* 10 registers */

    lv_obj_t *read_btn = ui_button(card, LV_SYMBOL_DOWNLOAD " Read", do_read, NULL);
    lv_obj_set_size(read_btn, (w - 24) / 2, 28);
    lv_obj_set_pos(read_btn, 0, 114);

    lv_obj_t *write_btn = ui_button_soft(card, LV_SYMBOL_UPLOAD " Write", do_write, NULL);
    lv_obj_set_size(write_btn, (w - 24) / 2, 28);
    lv_obj_set_pos(write_btn, (w - 24) / 2 + 8, 114);

    lv_obj_t *dl = ui_label(card, "Data (Hex)", &lv_font_montserrat_12, UI_COL_MUTED);
    lv_obj_set_pos(dl, 0, 150);

    s_data = lv_textarea_create(card);
    lv_obj_set_size(s_data, w - 12, h - 182);
    lv_obj_set_pos(s_data, 0, 168);
    lv_obj_set_style_text_font(s_data, &lv_font_montserrat_12, 0);
    lv_obj_set_style_border_width(s_data, 0, 0);
    lv_obj_set_style_bg_color(s_data, lv_color_hex(0xF7F9F8), 0);
    lv_textarea_set_text(s_data, "Pick a device, then Read.\n");
    lv_textarea_set_cursor_click_pos(s_data, false);
    lv_obj_clear_flag(s_data, LV_OBJ_FLAG_CLICK_FOCUSABLE);
}

static void on_enter(void)
{
    lv_obj_t *slot = ui_header_slot(UI_SCREEN_MODBUS);
    if (slot && !s_pill) {
        s_pill = ui_pill(slot, "Offline", UI_COL_MUTED);
    }

    const app_link_state_t st = svc_modbus_link_state();
    on_state(NULL, NULL, 0, (void *)&st);

    app_event_subscribe(APP_EVT_MODBUS_RESULT, on_result, NULL);
    app_event_subscribe(APP_EVT_MODBUS_STATE,  on_state,  NULL);
}

static void on_leave(void)
{
    app_event_unsubscribe(APP_EVT_MODBUS_RESULT, on_result);
    app_event_unsubscribe(APP_EVT_MODBUS_STATE,  on_state);
    wdialog_close(NULL);
}

const ui_screen_def_t ui_screen_modbus_def = {
    .title    = "Modbus",
    .create   = create,
    .on_enter = on_enter,
    .on_leave = on_leave,
};
