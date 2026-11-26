/*
 * NEX firmware — system UI
 * SPDX-License-Identifier: MIT
 *
 * Rust owns page/selection/clock state; this file owns every LVGL object and
 * the UI task. Pages are created once at startup and toggled, so repeated
 * navigation never allocates new page trees.
 */
#include "ui.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "board.h"
#include "esp_app_desc.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "nex_core.h"

#ifndef NEX_GIT_COMMIT
#define NEX_GIT_COMMIT "unknown"
#endif

namespace nex {
namespace {

constexpr const char *TAG = "NEX-UI";

constexpr int kPageCount = 8;
constexpr uint32_t kTickMs = 20;

SemaphoreHandle_t s_lvgl_mutex = nullptr;
QueueHandle_t s_key_queue = nullptr;
lv_obj_t *s_pages[kPageCount] = {};
int s_current_page = -1;
char s_digits[8] = "";

// Watchface
lv_obj_t *s_time_h = nullptr;
lv_obj_t *s_time_m = nullptr;
lv_obj_t *s_time_s = nullptr;
lv_obj_t *s_time_invalid = nullptr;

// Settings / developer / apps rows
lv_obj_t *s_settings_rows[4] = {};
lv_obj_t *s_dev_rows[5] = {};
lv_obj_t *s_dev_mode_label = nullptr;
lv_obj_t *s_app_rows[4] = {};
lv_obj_t *s_apps_error = nullptr;

// System info / input diagnostics
lv_obj_t *s_sysinfo_label = nullptr;
lv_obj_t *s_input_label = nullptr;

// Time editor
struct EditState {
    int year = 2026;
    int month = 1;
    int day = 1;
    int hour = 0;
    int minute = 0;
    int second = 0;
    int tz = 480;
    int field = 0;  // 0=Y 1=M 2=D 3=h 4=m 5=s 6=tz
    char error[96] = {};
};
EditState s_edit;
lv_obj_t *s_field_buttons[7] = {};
lv_obj_t *s_field_labels[7] = {};
lv_obj_t *s_edit_error = nullptr;

// App runner
lv_obj_t *s_app_title = nullptr;
lv_obj_t *s_app_detail = nullptr;

// Input diagnostics ring buffer
struct InputEvent {
    uint32_t uptime_s;
    char text[40];
};
InputEvent s_events[8] = {};
int s_event_count = 0;
int s_event_next = 0;

// ---------------------------------------------------------------- utilities
void set_text_if_changed(lv_obj_t *label, const char *text)
{
    if (label == nullptr) {
        return;
    }
    const char *current = lv_label_get_text(label);
    if (current == nullptr || std::strcmp(current, text) != 0) {
        lv_label_set_text(label, text);
    }
}

void add_event(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void add_event(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    char buffer[40];
    std::vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);

    InputEvent &e = s_events[s_event_next];
    e.uptime_s = static_cast<uint32_t>(esp_timer_get_time() / 1000000);
    std::snprintf(e.text, sizeof(e.text), "%s", buffer);
    s_event_next = (s_event_next + 1) % 8;
    if (s_event_count < 8) {
        ++s_event_count;
    }
}

const char *clock_source_name(int32_t source)
{
    switch (source) {
        case NEX_CLOCK_MANUAL:
            return "MANUAL";
        case NEX_CLOCK_RTC:
            return "RTC";
        case NEX_CLOCK_BLUETOOTH:
            return "BT";
        case NEX_CLOCK_WIFI:
            return "WIFI";
        default:
            return "NOT SET";
    }
}

const char *page_name(int32_t page);

const char *page_title(int32_t page)
{
    switch (page) {
        case NEX_PAGE_TIME_EDITOR:
            return "TIME & DATE";
        case NEX_PAGE_INPUT_DIAG:
            return "INPUT EVENTS";
        case NEX_PAGE_APPS:
            return "BUILT-IN APPS";
        default:
            return page_name(page);
    }
}

const char *page_name(int32_t page)
{
    switch (page) {
        case NEX_PAGE_WATCHFACE:
            return "WATCHFACE";
        case NEX_PAGE_SETTINGS:
            return "SETTINGS";
        case NEX_PAGE_TIME_EDITOR:
            return "TIME EDITOR";
        case NEX_PAGE_SYSTEM_INFO:
            return "SYSTEM INFO";
        case NEX_PAGE_DEVELOPER:
            return "DEVELOPER";
        case NEX_PAGE_INPUT_DIAG:
            return "INPUT DIAG";
        case NEX_PAGE_APPS:
            return "APPS";
        case NEX_PAGE_APP_RUNNER:
            return "APP";
        default:
            return "?";
    }
}

lv_obj_t *make_page(const char *title)
{
    lv_obj_t *page = lv_obj_create(lv_screen_active());
    lv_obj_set_size(page, 468, 466);
    lv_obj_set_style_bg_color(page, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(page, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(page, 0, 0);
    lv_obj_set_style_pad_all(page, 0, 0);
    lv_obj_remove_flag(page, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(page, LV_OBJ_FLAG_HIDDEN);

    if (title != nullptr) {
        lv_obj_t *label = lv_label_create(page);
        lv_label_set_text(label, title);
        lv_obj_set_style_text_font(label, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(label, lv_color_white(), 0);
        lv_obj_align(label, LV_ALIGN_TOP_MID, 0, 22);
    }
    return page;
}

lv_obj_t *make_row(lv_obj_t *parent, int y, int action, int arg, const char *text, int font)
{
    lv_obj_t *row = lv_button_create(parent);
    lv_obj_set_size(row, 400, 54);
    lv_obj_align(row, LV_ALIGN_TOP_MID, 0, y);
    lv_obj_set_style_radius(row, 12, 0);
    lv_obj_set_style_bg_color(row, lv_color_hex(0x0D0D0D), 0);
    lv_obj_set_style_border_width(row, 1, 0);
    lv_obj_set_style_border_color(row, lv_color_hex(0x333333), 0);
    lv_obj_set_style_shadow_width(row, 0, 0);

    lv_obj_t *label = lv_label_create(row);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, font == 20 ? &lv_font_montserrat_20 : &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    lv_obj_align(label, LV_ALIGN_LEFT_MID, 18, 0);
    return row;
}

void style_row_selected(lv_obj_t *row, bool selected)
{
    lv_obj_set_style_border_width(row, selected ? 2 : 1, 0);
    lv_obj_set_style_border_color(row, lv_color_hex(selected ? 0xFFFFFF : 0x333333), 0);
    lv_obj_set_style_bg_color(row, lv_color_hex(selected ? 0x1E1E1E : 0x0D0D0D), 0);
}

void set_row_label(lv_obj_t *row, const char *text, int font)
{
    lv_obj_t *label = lv_obj_get_child(row, 0);
    if (label != nullptr) {
        lv_label_set_text(label, text);
        lv_obj_set_style_text_font(label, font == 20 ? &lv_font_montserrat_20 : &lv_font_montserrat_16, 0);
    }
}

// ------------------------------------------------------------------ actions
void ui_sync();

void row_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) {
        return;
    }
    const intptr_t code = reinterpret_cast<intptr_t>(lv_event_get_user_data(event));
    const int action = static_cast<int>(code / 1000);
    const int arg = static_cast<int>(code % 1000);

    if (action == NEX_ACT_SELECT) {
        add_event("TOUCH %s[%d]", page_name(nex_core_page()), arg);
        nex_core_action(NEX_ACT_SELECT, arg);
        nex_core_action(NEX_ACT_CONFIRM, 0);
    } else {
        add_event("TOUCH %s act%d", page_name(nex_core_page()), action);
        nex_core_action(action, arg);
    }
    ui_sync();
}

// ----------------------------------------------------------------- time edit
int days_in_month(int year, int month)
{
    static const int days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month == 2 && ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0)) {
        return 29;
    }
    if (month < 1 || month > 12) {
        return 31;
    }
    return days[month - 1];
}

void clamp_edit_day()
{
    const int max_day = days_in_month(s_edit.year, s_edit.month);
    if (s_edit.day > max_day) {
        s_edit.day = max_day;
    }
    if (s_edit.day < 1) {
        s_edit.day = 1;
    }
}

void adjust_field(int delta)
{
    switch (s_edit.field) {
        case 0:
            s_edit.year += delta;
            if (s_edit.year < 2000) s_edit.year = 2099;
            if (s_edit.year > 2099) s_edit.year = 2000;
            clamp_edit_day();
            break;
        case 1:
            s_edit.month += delta;
            if (s_edit.month < 1) s_edit.month = 12;
            if (s_edit.month > 12) s_edit.month = 1;
            clamp_edit_day();
            break;
        case 2:
            s_edit.day += delta;
            if (s_edit.day < 1) s_edit.day = days_in_month(s_edit.year, s_edit.month);
            if (s_edit.day > days_in_month(s_edit.year, s_edit.month)) s_edit.day = 1;
            break;
        case 3:
            s_edit.hour = (s_edit.hour + delta + 24) % 24;
            break;
        case 4:
            s_edit.minute = (s_edit.minute + delta + 60) % 60;
            break;
        case 5:
            s_edit.second = (s_edit.second + delta + 60) % 60;
            break;
        case 6:
            s_edit.tz += delta * 30;
            if (s_edit.tz < -720) s_edit.tz = 840;
            if (s_edit.tz > 840) s_edit.tz = -720;
            break;
        default:
            break;
    }
}

void field_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) {
        return;
    }
    s_edit.field = static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(event)));
    add_event("TOUCH field %d", s_edit.field);
    ui_sync();
}

void adjust_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) {
        return;
    }
    const int delta = static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(event)));
    adjust_field(delta);
    s_edit.error[0] = 0;
    ui_sync();
}

void save_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) {
        return;
    }
    const int rc = nex_core_set_time(s_edit.year, s_edit.month, s_edit.day, s_edit.hour,
                                     s_edit.minute, s_edit.second, s_edit.tz);
    if (rc == 0) {
        ESP_LOGI(TAG, "time set to %04d-%02d-%02d %02d:%02d:%02d tz=%+d min", s_edit.year, s_edit.month,
                 s_edit.day, s_edit.hour, s_edit.minute, s_edit.second, s_edit.tz);
        add_event("TIME SAVED");
        nex_core_action(NEX_ACT_BACK, 0);
        s_edit.error[0] = 0;
        ui_sync();
    } else {
        char message[64] = {};
        nex_core_last_error(message, sizeof(message));
        std::snprintf(s_edit.error, sizeof(s_edit.error), "%.*s (rc %d)", 60, message, rc);
        add_event("TIME SAVE FAILED");
        ui_sync();
    }
}

void cancel_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) {
        return;
    }
    add_event("TIME EDIT CANCEL");
    nex_core_action(NEX_ACT_BACK, 0);
    ui_sync();
}

// ------------------------------------------------------------------- pages
void build_watchface()
{
    lv_obj_t *page = s_pages[NEX_PAGE_WATCHFACE];

    lv_obj_t *row = lv_obj_create(page);
    lv_obj_set_size(row, 460, 90);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_style_pad_column(row, 8, 0);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER);
    lv_obj_align(row, LV_ALIGN_CENTER, 0, -20);

    auto make_digit = [&](int width, const lv_font_t *font, bool bold) {
        lv_obj_t *label = lv_label_create(row);
        lv_obj_set_width(label, width);
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_font(label, font, 0);
        lv_obj_set_style_text_color(label, lv_color_white(), 0);
        lv_label_set_text(label, "00");
        if (bold) {
            lv_obj_set_style_text_outline_stroke_width(label, 2, 0);
            lv_obj_set_style_text_outline_stroke_color(label, lv_color_white(), 0);
        }
        return label;
    };
    s_time_h = make_digit(66, &lv_font_montserrat_48, true);
    s_time_m = make_digit(66, &lv_font_montserrat_48, false);
    s_time_s = make_digit(42, &lv_font_montserrat_28, false);

    s_time_invalid = lv_label_create(page);
    lv_label_set_text(s_time_invalid, "------");
    lv_obj_set_style_text_font(s_time_invalid, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(s_time_invalid, lv_color_white(), 0);
    lv_obj_align(s_time_invalid, LV_ALIGN_CENTER, 0, -20);
    lv_obj_add_flag(s_time_invalid, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *hint = lv_label_create(page);
    lv_label_set_text(hint, "HOLD KEYB: SETTINGS");
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(hint, lv_color_hex(0x777777), 0);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -40);
}

void build_settings()
{
    lv_obj_t *page = s_pages[NEX_PAGE_SETTINGS];
    static const char *items[] = {"Time & Date", "System Info", "Developer Options", "Back to Watchface"};
    for (int i = 0; i < 4; ++i) {
        s_settings_rows[i] = make_row(page, 96 + i * 64, NEX_ACT_SELECT, i, items[i], 20);
        lv_obj_add_event_cb(s_settings_rows[i], row_event_cb, LV_EVENT_CLICKED,
                            reinterpret_cast<void *>(static_cast<intptr_t>(NEX_ACT_SELECT * 1000 + i)));
    }
}

void build_time_editor()
{
    lv_obj_t *page = s_pages[NEX_PAGE_TIME_EDITOR];

    lv_obj_t *row1 = lv_obj_create(page);
    lv_obj_set_size(row1, 440, 64);
    lv_obj_set_style_bg_opa(row1, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row1, 0, 0);
    lv_obj_set_style_pad_all(row1, 0, 0);
    lv_obj_set_style_pad_column(row1, 8, 0);
    lv_obj_remove_flag(row1, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(row1, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row1, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_align(row1, LV_ALIGN_TOP_MID, 0, 86);

    lv_obj_t *row2 = lv_obj_create(page);
    lv_obj_set_size(row2, 440, 64);
    lv_obj_set_style_bg_opa(row2, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row2, 0, 0);
    lv_obj_set_style_pad_all(row2, 0, 0);
    lv_obj_set_style_pad_column(row2, 8, 0);
    lv_obj_remove_flag(row2, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(row2, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row2, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_align(row2, LV_ALIGN_TOP_MID, 0, 158);

    static const int widths[7] = {128, 92, 92, 82, 82, 82, 128};
    for (int i = 0; i < 7; ++i) {
        lv_obj_t *parent = i < 3 ? row1 : row2;
        lv_obj_t *button = lv_button_create(parent);
        lv_obj_set_size(button, widths[i], 58);
        lv_obj_set_style_radius(button, 10, 0);
        lv_obj_set_style_bg_color(button, lv_color_hex(0x0D0D0D), 0);
        lv_obj_set_style_border_width(button, 1, 0);
        lv_obj_set_style_border_color(button, lv_color_hex(0x333333), 0);
        lv_obj_set_style_shadow_width(button, 0, 0);
        lv_obj_add_event_cb(button, field_event_cb, LV_EVENT_CLICKED,
                            reinterpret_cast<void *>(static_cast<intptr_t>(i)));

        lv_obj_t *label = lv_label_create(button);
        lv_obj_set_style_text_font(label, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(label, lv_color_white(), 0);
        lv_label_set_text(label, "00");
        lv_obj_center(label);

        s_field_buttons[i] = button;
        s_field_labels[i] = label;
    }

    lv_obj_t *minus = lv_button_create(page);
    lv_obj_set_size(minus, 100, 70);
    lv_obj_align(minus, LV_ALIGN_TOP_LEFT, 70, 244);
    lv_obj_set_style_radius(minus, 12, 0);
    lv_obj_set_style_bg_color(minus, lv_color_hex(0x0D0D0D), 0);
    lv_obj_set_style_border_width(minus, 1, 0);
    lv_obj_set_style_border_color(minus, lv_color_hex(0x333333), 0);
    lv_obj_add_event_cb(minus, adjust_event_cb, LV_EVENT_CLICKED,
                        reinterpret_cast<void *>(static_cast<intptr_t>(-1)));
    lv_obj_t *minus_label = lv_label_create(minus);
    lv_label_set_text(minus_label, "-");
    lv_obj_set_style_text_font(minus_label, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(minus_label, lv_color_white(), 0);
    lv_obj_center(minus_label);

    lv_obj_t *plus = lv_button_create(page);
    lv_obj_set_size(plus, 100, 70);
    lv_obj_align(plus, LV_ALIGN_TOP_RIGHT, -70, 244);
    lv_obj_set_style_radius(plus, 12, 0);
    lv_obj_set_style_bg_color(plus, lv_color_hex(0x0D0D0D), 0);
    lv_obj_set_style_border_width(plus, 1, 0);
    lv_obj_set_style_border_color(plus, lv_color_hex(0x333333), 0);
    lv_obj_add_event_cb(plus, adjust_event_cb, LV_EVENT_CLICKED,
                        reinterpret_cast<void *>(static_cast<intptr_t>(1)));
    lv_obj_t *plus_label = lv_label_create(plus);
    lv_label_set_text(plus_label, "+");
    lv_obj_set_style_text_font(plus_label, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(plus_label, lv_color_white(), 0);
    lv_obj_center(plus_label);

    s_edit_error = lv_label_create(page);
    lv_obj_set_width(s_edit_error, 420);
    lv_obj_set_style_text_align(s_edit_error, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(s_edit_error, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(s_edit_error, lv_color_hex(0xFF8080), 0);
    lv_obj_align(s_edit_error, LV_ALIGN_TOP_MID, 0, 330);
    lv_label_set_text(s_edit_error, "");

    lv_obj_t *cancel = lv_button_create(page);
    lv_obj_set_size(cancel, 150, 54);
    lv_obj_align(cancel, LV_ALIGN_BOTTOM_LEFT, 40, -34);
    lv_obj_set_style_radius(cancel, 12, 0);
    lv_obj_set_style_bg_color(cancel, lv_color_hex(0x0D0D0D), 0);
    lv_obj_set_style_border_width(cancel, 1, 0);
    lv_obj_set_style_border_color(cancel, lv_color_hex(0x333333), 0);
    lv_obj_add_event_cb(cancel, cancel_event_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *cancel_label = lv_label_create(cancel);
    lv_label_set_text(cancel_label, "Cancel");
    lv_obj_set_style_text_font(cancel_label, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(cancel_label, lv_color_white(), 0);
    lv_obj_center(cancel_label);

    lv_obj_t *save = lv_button_create(page);
    lv_obj_set_size(save, 150, 54);
    lv_obj_align(save, LV_ALIGN_BOTTOM_RIGHT, -40, -34);
    lv_obj_set_style_radius(save, 12, 0);
    lv_obj_set_style_bg_color(save, lv_color_hex(0x1E1E1E), 0);
    lv_obj_set_style_border_width(save, 2, 0);
    lv_obj_set_style_border_color(save, lv_color_white(), 0);
    lv_obj_add_event_cb(save, save_event_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *save_label = lv_label_create(save);
    lv_label_set_text(save_label, "Save");
    lv_obj_set_style_text_font(save_label, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(save_label, lv_color_white(), 0);
    lv_obj_center(save_label);
}

void build_system_info()
{
    lv_obj_t *page = s_pages[NEX_PAGE_SYSTEM_INFO];
    s_sysinfo_label = lv_label_create(page);
    lv_obj_set_width(s_sysinfo_label, 420);
    lv_obj_set_style_text_font(s_sysinfo_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_sysinfo_label, lv_color_white(), 0);
    lv_label_set_long_mode(s_sysinfo_label, LV_LABEL_LONG_WRAP);
    lv_obj_align(s_sysinfo_label, LV_ALIGN_TOP_LEFT, 24, 70);
    lv_label_set_text(s_sysinfo_label, "");

    lv_obj_t *back = lv_button_create(page);
    lv_obj_set_size(back, 160, 50);
    lv_obj_align(back, LV_ALIGN_BOTTOM_MID, 0, -28);
    lv_obj_set_style_radius(back, 12, 0);
    lv_obj_set_style_bg_color(back, lv_color_hex(0x0D0D0D), 0);
    lv_obj_set_style_border_width(back, 1, 0);
    lv_obj_set_style_border_color(back, lv_color_hex(0x333333), 0);
    lv_obj_add_event_cb(back, row_event_cb, LV_EVENT_CLICKED,
                        reinterpret_cast<void *>(static_cast<intptr_t>(NEX_ACT_BACK * 1000)));
    lv_obj_t *label = lv_label_create(back);
    lv_label_set_text(label, "Back");
    lv_obj_set_style_text_font(label, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    lv_obj_center(label);
}

void build_developer()
{
    lv_obj_t *page = s_pages[NEX_PAGE_DEVELOPER];
    static const char *items[] = {"Developer mode", "System Status", "Input Diagnostics", "Built-in Apps",
                                  "Back to Settings"};
    for (int i = 0; i < 5; ++i) {
        s_dev_rows[i] = make_row(page, 78 + i * 62, NEX_ACT_SELECT, i, items[i], 16);
        lv_obj_add_event_cb(s_dev_rows[i], row_event_cb, LV_EVENT_CLICKED,
                            reinterpret_cast<void *>(static_cast<intptr_t>(NEX_ACT_SELECT * 1000 + i)));
        if (i == 0) {
            s_dev_mode_label = lv_obj_get_child(s_dev_rows[i], 0);
        }
    }
    set_row_label(s_dev_rows[0], "Developer mode: OFF", 16);
}

void build_input_diag()
{
    lv_obj_t *page = s_pages[NEX_PAGE_INPUT_DIAG];
    s_input_label = lv_label_create(page);
    lv_obj_set_width(s_input_label, 420);
    lv_obj_set_style_text_font(s_input_label, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(s_input_label, lv_color_white(), 0);
    lv_obj_set_style_text_line_space(s_input_label, 4, 0);
    lv_obj_align(s_input_label, LV_ALIGN_TOP_LEFT, 24, 70);
    lv_label_set_text(s_input_label, "");

    lv_obj_t *back = lv_button_create(page);
    lv_obj_set_size(back, 160, 50);
    lv_obj_align(back, LV_ALIGN_BOTTOM_MID, 0, -28);
    lv_obj_set_style_radius(back, 12, 0);
    lv_obj_set_style_bg_color(back, lv_color_hex(0x0D0D0D), 0);
    lv_obj_set_style_border_width(back, 1, 0);
    lv_obj_set_style_border_color(back, lv_color_hex(0x333333), 0);
    lv_obj_add_event_cb(back, row_event_cb, LV_EVENT_CLICKED,
                        reinterpret_cast<void *>(static_cast<intptr_t>(NEX_ACT_BACK * 1000)));
    lv_obj_t *label = lv_label_create(back);
    lv_label_set_text(label, "Back");
    lv_obj_set_style_text_font(label, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    lv_obj_center(label);
}

void build_apps()
{
    lv_obj_t *page = s_pages[NEX_PAGE_APPS];
    for (int i = 0; i < 4; ++i) {
        s_app_rows[i] = make_row(page, 78 + i * 62, NEX_ACT_SELECT, i, "", 16);
        lv_obj_add_event_cb(s_app_rows[i], row_event_cb, LV_EVENT_CLICKED,
                            reinterpret_cast<void *>(static_cast<intptr_t>(NEX_ACT_SELECT * 1000 + i)));
    }

    lv_obj_t *error = lv_label_create(page);
    lv_obj_set_width(error, 420);
    lv_obj_set_style_text_align(error, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(error, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(error, lv_color_hex(0xFF8080), 0);
    lv_obj_align(error, LV_ALIGN_BOTTOM_MID, 0, -16);
    lv_label_set_text(error, "");
    s_apps_error = error;
}

void build_app_runner()
{
    lv_obj_t *page = s_pages[NEX_PAGE_APP_RUNNER];
    lv_obj_t *running = lv_label_create(page);
    lv_label_set_text(running, "APP RUNNING");
    lv_obj_set_style_text_font(running, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(running, lv_color_hex(0x9A9AA2), 0);
    lv_obj_align(running, LV_ALIGN_TOP_MID, 0, 96);

    s_app_title = lv_label_create(page);
    lv_obj_set_style_text_font(s_app_title, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(s_app_title, lv_color_white(), 0);
    lv_obj_align(s_app_title, LV_ALIGN_CENTER, 0, -40);
    lv_label_set_text(s_app_title, "");

    s_app_detail = lv_label_create(page);
    lv_obj_set_style_text_font(s_app_detail, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_app_detail, lv_color_hex(0xBBBBBB), 0);
    lv_obj_set_style_text_align(s_app_detail, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_app_detail, LV_ALIGN_CENTER, 0, 30);
    lv_label_set_text(s_app_detail, "");

    lv_obj_t *back = lv_button_create(page);
    lv_obj_set_size(back, 160, 50);
    lv_obj_align(back, LV_ALIGN_BOTTOM_MID, 0, -34);
    lv_obj_set_style_radius(back, 12, 0);
    lv_obj_set_style_bg_color(back, lv_color_hex(0x0D0D0D), 0);
    lv_obj_set_style_border_width(back, 1, 0);
    lv_obj_set_style_border_color(back, lv_color_hex(0x333333), 0);
    lv_obj_add_event_cb(back, row_event_cb, LV_EVENT_CLICKED,
                        reinterpret_cast<void *>(static_cast<intptr_t>(NEX_ACT_BACK * 1000)));
    lv_obj_t *label = lv_label_create(back);
    lv_label_set_text(label, "Exit App");
    lv_obj_set_style_text_font(label, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    lv_obj_center(label);
}

// ---------------------------------------------------------------- refreshing
void refresh_watchface()
{
    char digits[8] = {};
    nex_core_time_digits(digits);
    if (std::strcmp(digits, s_digits) == 0) {
        return;
    }
    std::snprintf(s_digits, sizeof(s_digits), "%s", digits);

    const bool invalid = digits[0] == '-';
    if (invalid) {
        lv_obj_add_flag(s_time_h, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_time_m, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_time_s, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_time_invalid, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    lv_obj_remove_flag(s_time_h, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s_time_m, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s_time_s, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_time_invalid, LV_OBJ_FLAG_HIDDEN);

    char part[3] = {digits[0], digits[1], 0};
    lv_label_set_text(s_time_h, part);
    part[0] = digits[2];
    part[1] = digits[3];
    lv_label_set_text(s_time_m, part);
    part[0] = digits[4];
    part[1] = digits[5];
    lv_label_set_text(s_time_s, part);
}

void refresh_settings()
{
    const int sel = nex_core_selection();
    for (int i = 0; i < 4; ++i) {
        style_row_selected(s_settings_rows[i], i == sel);
    }
}

void refresh_system_info()
{
    const esp_app_desc_t *app = esp_app_get_description();
    const uint32_t free_heap = esp_get_free_heap_size() / 1024;
    const uint32_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024;
    const uint64_t uptime = static_cast<uint64_t>(esp_timer_get_time() / 1000000);
    char buffer[520];
    std::snprintf(buffer, sizeof(buffer),
                  "Firmware   %s\n"
                  "App API    %ld\n"
                  "Commit     %s\n"
                  "Free heap  %lu KB\n"
                  "Free PSRAM %lu KB\n"
                  "Uptime     %02llu:%02llu:%02llu\n"
                  "Clock      %s (%s)\n"
                  "Status     %s",
                  app->version, static_cast<long>(nex_core_api_version()), NEX_GIT_COMMIT,
                  static_cast<unsigned long>(free_heap), static_cast<unsigned long>(free_psram),
                  static_cast<unsigned long long>(uptime / 3600),
                  static_cast<unsigned long long>((uptime / 60) % 60),
                  static_cast<unsigned long long>(uptime % 60), clock_source_name(nex_core_clock_source()),
                  nex_core_clock_valid() ? "valid" : "not set",
                  nex_core_clock_valid() ? "OK" : "SET TIME MANUALLY");
    set_text_if_changed(s_sysinfo_label, buffer);
}

void refresh_developer()
{
    const int sel = nex_core_selection();
    for (int i = 0; i < 5; ++i) {
        style_row_selected(s_dev_rows[i], i == sel);
    }
    char mode[40];
    std::snprintf(mode, sizeof(mode), "Developer mode: %s", nex_core_dev_enabled() ? "ON" : "OFF");
    set_text_if_changed(s_dev_mode_label, mode);
}

void refresh_input_diag()
{
    char buffer[420] = {};
    int length = 0;
    for (int i = 0; i < s_event_count; ++i) {
        const int index = (s_event_next - 1 - i + 8) % 8;
        length += std::snprintf(buffer + length, sizeof(buffer) - length, "%lus  %s\n",
                                static_cast<unsigned long>(s_events[index].uptime_s),
                                s_events[index].text);
        if (length >= static_cast<int>(sizeof(buffer)) - 2) {
            break;
        }
    }
    if (s_event_count == 0) {
        std::snprintf(buffer, sizeof(buffer), "no input events yet");
    }
    set_text_if_changed(s_input_label, buffer);
}

void refresh_apps()
{
    const int count = nex_core_app_count();
    const int sel = nex_core_selection();
    for (int i = 0; i < 4; ++i) {
        if (i < count) {
            nex_app_info_t info = {};
            nex_core_app_info(i, &info);
            char text[96];
            std::snprintf(text, sizeof(text), "%s v%lu  API%lu  %s", info.name,
                          static_cast<unsigned long>(info.version),
                          static_cast<unsigned long>(info.required_api),
                          info.compatible ? "OK" : "INCOMPAT");
            set_row_label(s_app_rows[i], text, 16);
            lv_obj_remove_flag(s_app_rows[i], LV_OBJ_FLAG_HIDDEN);
        } else if (i == count) {
            set_row_label(s_app_rows[i], "Back to Developer", 16);
            lv_obj_remove_flag(s_app_rows[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_app_rows[i], LV_OBJ_FLAG_HIDDEN);
        }
        style_row_selected(s_app_rows[i], i == sel);
    }

    char message[64] = {};
    nex_core_last_error(message, sizeof(message));
    if (s_apps_error != nullptr) {
        set_text_if_changed(s_apps_error, message);
    }
}

void refresh_time_editor()
{
    char year[8];
    std::snprintf(year, sizeof(year), "%04d", s_edit.year);
    lv_label_set_text(s_field_labels[0], year);

    char part[16];
    std::snprintf(part, sizeof(part), "%02d", s_edit.month);
    lv_label_set_text(s_field_labels[1], part);
    std::snprintf(part, sizeof(part), "%02d", s_edit.day);
    lv_label_set_text(s_field_labels[2], part);
    std::snprintf(part, sizeof(part), "%02d", s_edit.hour);
    lv_label_set_text(s_field_labels[3], part);
    std::snprintf(part, sizeof(part), "%02d", s_edit.minute);
    lv_label_set_text(s_field_labels[4], part);
    std::snprintf(part, sizeof(part), "%02d", s_edit.second);
    lv_label_set_text(s_field_labels[5], part);

    const int tz_abs = s_edit.tz < 0 ? -s_edit.tz : s_edit.tz;
    std::snprintf(part, sizeof(part), "%c%02d:%02d", s_edit.tz < 0 ? '-' : '+', tz_abs / 60, tz_abs % 60);
    lv_label_set_text(s_field_labels[6], part);

    for (int i = 0; i < 7; ++i) {
        style_row_selected(s_field_buttons[i], i == s_edit.field);
    }
    set_text_if_changed(s_edit_error, s_edit.error);
}

void enter_time_editor()
{
    nex_civil_t fields = {};
    nex_core_local_fields(&fields);
    s_edit.year = fields.year;
    s_edit.month = fields.month;
    s_edit.day = fields.day;
    s_edit.hour = fields.hour;
    s_edit.minute = fields.minute;
    s_edit.second = fields.second;
    s_edit.tz = fields.tz_minutes;
    s_edit.field = 0;
    s_edit.error[0] = 0;

    // When the clock has never been set, start the editor at the build time so
    // the user only has to confirm (or tweak) instead of scrolling from 2026-01-01.
    if (nex_core_clock_valid() == 0) {
        char month_name[4] = {};
        int day = 0;
        int year = 0;
        if (std::sscanf(__DATE__, "%3s %d %d", month_name, &day, &year) == 3) {
            static const char *months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                           "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
            int month = 0;
            for (int i = 0; i < 12; ++i) {
                if (std::strcmp(month_name, months[i]) == 0) {
                    month = i + 1;
                    break;
                }
            }
            int hour = 0;
            int minute = 0;
            int second = 0;
            std::sscanf(__TIME__, "%d:%d:%d", &hour, &minute, &second);
            if (month != 0 && year >= 2000 && year <= 2099 && day >= 1 && day <= 31) {
                s_edit.year = year;
                s_edit.month = month;
                s_edit.day = day;
                s_edit.hour = hour;
                s_edit.minute = minute;
                s_edit.second = second;
            }
        }
    }
}

void refresh_app_runner()
{
    const int index = nex_core_running_app();
    nex_app_info_t info = {};
    if (index >= 0 && nex_core_app_info(index, &info) == 0) {
        set_text_if_changed(s_app_title, info.name);
        char detail[128];
        std::snprintf(detail, sizeof(detail), "%s\nentry: %s\nAPI required: %lu",
                      info.id, info.entry, static_cast<unsigned long>(info.required_api));
        set_text_if_changed(s_app_detail, detail);
    } else {
        set_text_if_changed(s_app_title, "No app");
        set_text_if_changed(s_app_detail, "");
    }
}

void refresh_page(int page)
{
    switch (page) {
        case NEX_PAGE_WATCHFACE:
            refresh_watchface();
            break;
        case NEX_PAGE_SETTINGS:
            refresh_settings();
            break;
        case NEX_PAGE_TIME_EDITOR:
            refresh_time_editor();
            break;
        case NEX_PAGE_SYSTEM_INFO:
            refresh_system_info();
            break;
        case NEX_PAGE_DEVELOPER:
            refresh_developer();
            break;
        case NEX_PAGE_INPUT_DIAG:
            refresh_input_diag();
            break;
        case NEX_PAGE_APPS:
            refresh_apps();
            break;
        case NEX_PAGE_APP_RUNNER:
            refresh_app_runner();
            break;
        default:
            break;
    }
}

void show_page(int page)
{
    if (page < 0 || page >= kPageCount) {
        page = NEX_PAGE_WATCHFACE;
    }
    for (int i = 0; i < kPageCount; ++i) {
        if (s_pages[i] != nullptr) {
            if (i == page) {
                lv_obj_remove_flag(s_pages[i], LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_add_flag(s_pages[i], LV_OBJ_FLAG_HIDDEN);
            }
        }
    }
    s_current_page = page;
    ESP_LOGI(TAG, "page -> %s", page_name(page));
    if (page == NEX_PAGE_TIME_EDITOR) {
        enter_time_editor();
    }
    refresh_page(page);
}

void ui_sync()
{
    const int page = nex_core_page();
    if (page != s_current_page) {
        show_page(page);
    } else {
        refresh_page(page);
    }
}

// ---------------------------------------------------------------------- task
void ui_task(void *arg)
{
    (void)arg;
    TickType_t last_wake = xTaskGetTickCount();

    while (true) {
        int32_t event = 0;
        bool dirty = false;
        while (xQueueReceive(s_key_queue, &event, 0) == pdTRUE) {
            add_event("KEY %s", event == NEX_KEY_A_CLICK   ? "A CLICK"
                                : event == NEX_KEY_B_CLICK ? "B CLICK"
                                : event == NEX_KEY_A_LONG  ? "A LONG"
                                : event == NEX_KEY_B_LONG  ? "B LONG"
                                                           : "BOTH LONG");
            nex_core_key_event(event);
            dirty = true;
        }

        nex_core_tick(nex_monotonic_ms());

        if (xSemaphoreTake(s_lvgl_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            if (dirty) {
                ui_sync();
            }
            if (s_current_page == NEX_PAGE_WATCHFACE) {
                refresh_watchface();
            }
            lv_timer_handler();
            xSemaphoreGive(s_lvgl_mutex);
        }
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(kTickMs));
    }
}

}  // namespace

bool ui_start()
{
    lv_init();

    lv_display_t *disp = lv_display_create(468, 466);
    if (disp == nullptr) {
        ESP_LOGE(TAG, "lv_display_create failed");
        return false;
    }

    constexpr size_t kBytesPerPixel = 2;
    const size_t buf_bytes = 468 * 40 * kBytesPerPixel;
    auto *buf1 = static_cast<uint8_t *>(heap_caps_malloc(buf_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    auto *buf2 = static_cast<uint8_t *>(heap_caps_malloc(buf_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (buf1 == nullptr || buf2 == nullptr) {
        ESP_LOGE(TAG, "draw buffer allocation failed");
        heap_caps_free(buf1);
        heap_caps_free(buf2);
        return false;
    }

    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_user_data(disp, &board_display());
    lv_display_set_flush_cb(disp, [](lv_display_t *d, const lv_area_t *area, uint8_t *px_map) {
        auto *gfx = static_cast<lgfx::LGFX_Device *>(lv_display_get_user_data(d));
        const int32_t w = area->x2 - area->x1 + 1;
        const int32_t h = area->y2 - area->y1 + 1;
        gfx->startWrite();
        gfx->setAddrWindow(area->x1, area->y1, w, h);
        gfx->writePixels(reinterpret_cast<const lgfx::rgb565_t *>(px_map), static_cast<uint32_t>(w) * h);
        gfx->endWrite();
        lv_display_flush_ready(d);
    });
    lv_display_set_buffers(disp, buf1, buf2, buf_bytes, LV_DISPLAY_RENDER_MODE_PARTIAL);

    lv_indev_t *indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_display(indev, disp);
    lv_indev_set_read_cb(indev, [](lv_indev_t *, lv_indev_data_t *data) {
        uint16_t x = 0;
        uint16_t y = 0;
        bool pressed = false;
        if (!board_touch_init() || !board_touch_read(x, y, pressed)) {
            data->state = LV_INDEV_STATE_RELEASED;
            return;
        }
        if (pressed) {
            data->point.x = x;
            data->point.y = y;
            data->state = LV_INDEV_STATE_PRESSED;
        } else {
            data->state = LV_INDEV_STATE_RELEASED;
        }
    });

    const esp_timer_create_args_t timer_args = {
        .callback = [](void *) { lv_tick_inc(2); },
        .arg = nullptr,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "lvgl_tick",
        .skip_unhandled_events = false,
    };
    esp_timer_handle_t tick = nullptr;
    if (esp_timer_create(&timer_args, &tick) != ESP_OK || esp_timer_start_periodic(tick, 2000) != ESP_OK) {
        ESP_LOGE(TAG, "LVGL tick timer failed");
        return false;
    }

    s_lvgl_mutex = xSemaphoreCreateMutex();
    s_key_queue = xQueueCreate(16, sizeof(int32_t));
    if (s_lvgl_mutex == nullptr || s_key_queue == nullptr) {
        ESP_LOGE(TAG, "UI synchronization objects failed");
        return false;
    }

    for (int i = 0; i < kPageCount; ++i) {
        s_pages[i] = make_page(i == NEX_PAGE_WATCHFACE ? nullptr : page_title(i));
    }
    build_watchface();
    build_settings();
    build_time_editor();
    build_system_info();
    build_developer();
    build_input_diag();
    build_apps();
    build_app_runner();

    show_page(NEX_PAGE_WATCHFACE);

    if (xTaskCreate(ui_task, "nex_ui", 16 * 1024, nullptr, 5, nullptr) != pdPASS) {
        ESP_LOGE(TAG, "UI task creation failed");
        return false;
    }
    return true;
}

void ui_post_key_event(int32_t event)
{
    if (s_key_queue == nullptr) {
        return;
    }
    xQueueSend(s_key_queue, &event, 0);
}

}  // namespace nex
