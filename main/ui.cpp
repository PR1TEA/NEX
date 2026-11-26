/*
 * NEX firmware — system UI
 * SPDX-License-Identifier: MIT
 *
 * Rust owns page/selection/clock state; this file owns every LVGL object and
 * the UI task. Pages are created once at startup and toggled, so repeated
 * navigation never allocates new page trees.
 */
#include "ui.h"

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "board.h"
#include "esp_app_desc.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "audio.h"
#include "ble_upload.h"
#include "imu.h"
#include "reminders.h"
#include "wallpaper.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "misc/cache/instance/lv_image_cache.h"
#include "nex_core.h"

#ifndef NEX_GIT_COMMIT
#define NEX_GIT_COMMIT "unknown"
#endif

// Digit-only Montserrat fonts generated for the watchface (64/64/40 px).
extern "C" const lv_font_t nex_font_64_bold;
extern "C" const lv_font_t nex_font_64_regular;
extern "C" const lv_font_t nex_font_40_regular;
extern "C" const lv_font_t nex_cjk_20;
extern "C" const lv_font_t nex_cjk_16;
extern "C" const lv_font_t nex_cjk_text_16;

namespace nex {
namespace {

constexpr const char *TAG = "NEX-UI";

constexpr int kPageCount = 14;
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
lv_point_precise_t s_wf_hour_pts[2] = {{230, 230}, {230, 120}};
lv_point_precise_t s_wf_min_pts[2] = {{230, 230}, {230, 80}};
lv_point_precise_t s_wf_tick_pts[60][2];

// Stopwatch
lv_obj_t *s_sw_time = nullptr;
lv_obj_t *s_sw_state = nullptr;

// App menu / level
lv_obj_t *s_menu_rows[6] = {};
lv_obj_t *s_wallpaper = nullptr;
int s_wall_gen = -1;
lv_obj_t *s_phone_status = nullptr;

// Reminders
lv_obj_t *s_reminder_list = nullptr;
lv_obj_t *s_reminder_empty = nullptr;
int s_reminder_gen = -1;
int s_reminder_sel = -1;
int s_reminder_count = -1;

// Countdown timer
lv_obj_t *s_timer_value = nullptr;
lv_obj_t *s_timer_state = nullptr;
int s_timer_duration_ms = 60000;
int s_timer_remaining_ms = 60000;
int64_t s_timer_end_ms = 0;
bool s_timer_running = false;
bool s_timer_alarm = false;
bool s_timer_finished = false;
int64_t s_timer_alarm_deadline = 0;
lv_obj_t *s_att_canvas = nullptr;
uint16_t *s_att_buf = nullptr;
int s_att_theme = -1;
lv_obj_t *s_aircraft[3] = {};
lv_obj_t *s_bank_marker = nullptr;
lv_point_precise_t s_bank_pts[9][2];
lv_point_precise_t s_bank_marker_pts[2] = {{125, 125}, {125, 125}};

struct AttPalette {
    uint32_t sky;
    uint32_t ground;
    uint32_t horizon;
    uint32_t ladder;
    uint32_t aircraft;
    uint32_t bank;
};
const AttPalette kAttPalettes[4] = {
    {0x2E6FD6, 0x7A4B2A, 0xFFFFFF, 0xFFFFFF, 0xFFD24A, 0xFFFFFF},
    {0x0B2A4A, 0x2A1A0A, 0x9CCBFF, 0x9CCBFF, 0xFFB020, 0x9CCBFF},
    {0x101010, 0x2A2A2A, 0xFFFFFF, 0xFFFFFF, 0xFFFFFF, 0xFFFFFF},
    {0x0060A0, 0xA05000, 0xFFFFFF, 0xFFFFFF, 0xFF4040, 0xFFFFFF},
};

uint16_t rgb565(uint32_t color)
{
    const uint8_t r = (color >> 16) & 0xFF;
    const uint8_t g = (color >> 8) & 0xFF;
    const uint8_t b = color & 0xFF;
    return static_cast<uint16_t>(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

void draw_attitude(float roll, float pitch, int theme)
{
    if (s_att_buf == nullptr) {
        return;
    }
    const AttPalette &palette = kAttPalettes[theme & 3];
    const uint16_t sky = rgb565(palette.sky);
    const uint16_t ground = rgb565(palette.ground);
    const uint16_t horizon = rgb565(palette.horizon);
    const uint16_t ladder = rgb565(palette.ladder);

    constexpr int kW = 220;
    constexpr int kH = 220;
    constexpr int kC = 110;
    constexpr float kRadius2 = 108.0f * 108.0f;
    const float rad = roll * 0.0174533f;
    const float sr = std::sin(rad);
    const float cr = std::cos(rad);
    const float pitch_px = pitch * 2.0f;

    for (int y = 0; y < kH; ++y) {
        const float dy = static_cast<float>(y - kC);
        for (int x = 0; x < kW; ++x) {
            const float dx = static_cast<float>(x - kC);
            if (dx * dx + dy * dy > kRadius2) {
                s_att_buf[y * kW + x] = 0;
                continue;
            }
            const float d = dx * (-sr) + dy * cr - pitch_px;
            s_att_buf[y * kW + x] = d < 0.0f ? sky : ground;
        }
    }

    auto put = [&](int x, int y, uint16_t color) {
        if (x >= 0 && x < kW && y >= 0 && y < kH) {
            const float dx = static_cast<float>(x - kC);
            const float dy = static_cast<float>(y - kC);
            if (dx * dx + dy * dy <= kRadius2) {
                s_att_buf[y * kW + x] = color;
            }
        }
    };

    if (std::fabs(cr) > 0.05f) {
        for (int dx = -105; dx <= 105; ++dx) {
            const float dy = (pitch_px + static_cast<float>(dx) * sr) / cr;
            const int x = kC + dx;
            const int y = static_cast<int>(std::lround(static_cast<float>(kC) + dy));
            put(x, y, horizon);
            put(x, y + 1, horizon);
        }
    }

    for (int offset = -40; offset <= 40; offset += 20) {
        if (offset == 0) {
            continue;
        }
        const float bx = static_cast<float>(offset) * (-sr);
        const float by = static_cast<float>(offset) * cr;
        for (int t = -40; t <= 40; ++t) {
            if (t > -12 && t < 12) {
                continue;
            }
            const int x = static_cast<int>(std::lround(kC + bx + static_cast<float>(t) * cr));
            const int y = static_cast<int>(std::lround(kC + by + static_cast<float>(t) * sr));
            put(x, y, ladder);
        }
    }

    if (s_att_canvas != nullptr) {
        lv_obj_invalidate(s_att_canvas);
    }
}

AttPalette palette_for(int theme)
{
    return kAttPalettes[theme & 3];
}

// Settings / developer / apps rows
lv_obj_t *s_settings_rows[5] = {};
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
            return "时间与日期";
        case NEX_PAGE_INPUT_DIAG:
            return "INPUT EVENTS";
        case NEX_PAGE_APPS:
            return "BUILT-IN APPS";
        case NEX_PAGE_STOPWATCH:
            return "秒表";
        case NEX_PAGE_MENU:
            return "应用";
        case NEX_PAGE_GYRO:
            return "姿态仪";
        case NEX_PAGE_PHONE:
            return "手机连接";
        case NEX_PAGE_REMINDERS:
            return "提醒事项";
        case NEX_PAGE_TIMER:
            return "计时器";
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
            return "设置";
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
        case NEX_PAGE_PHONE:
            return "PHONE";
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
        lv_obj_set_style_text_font(label, &nex_cjk_20, 0);
        lv_obj_set_style_text_color(label, lv_color_white(), 0);
        lv_obj_align(label, LV_ALIGN_TOP_MID, 0, 22);
    }
    return page;
}

lv_obj_t *make_row(lv_obj_t *parent, int y, int action, int arg, const char *text, int font)
{
    lv_obj_t *row = lv_button_create(parent);
    lv_obj_set_size(row, 380, 64);
    lv_obj_align(row, LV_ALIGN_TOP_MID, 0, y);
    lv_obj_set_style_radius(row, 32, 0);
    lv_obj_set_style_bg_color(row, lv_color_hex(0x0D0D0D), 0);
    lv_obj_set_style_border_width(row, 1, 0);
    lv_obj_set_style_border_color(row, lv_color_hex(0x333333), 0);
    lv_obj_set_style_shadow_width(row, 0, 0);

    lv_obj_t *label = lv_label_create(row);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, font == 20 ? &nex_cjk_20 : &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    lv_obj_set_width(label, 340);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    lv_obj_center(label);
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
        lv_obj_set_style_text_font(label, font == 20 ? &nex_cjk_20 : &lv_font_montserrat_16, 0);
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
        std::snprintf(s_edit.error, sizeof(s_edit.error), "保存失败 (rc %d)", rc);
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

    s_wallpaper = lv_image_create(page);
    lv_obj_align(s_wallpaper, LV_ALIGN_CENTER, 0, 0);
    if (wallpaper_exists()) {
        lv_image_set_src(s_wallpaper, wallpaper_lvgl_path());
    } else {
        lv_obj_add_flag(s_wallpaper, LV_OBJ_FLAG_HIDDEN);
    }

    lv_obj_t *dial = lv_obj_create(page);
    lv_obj_set_size(dial, 460, 460);
    lv_obj_align(dial, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_opa(dial, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(dial, 0, 0);
    lv_obj_set_style_pad_all(dial, 0, 0);
    lv_obj_remove_flag(dial, LV_OBJ_FLAG_SCROLLABLE);

    constexpr float kCenter = 230.0f;
    constexpr float kPi = 3.14159265f;
    for (int i = 0; i < 60; ++i) {
        const float angle = (static_cast<float>(i) * 6.0f - 90.0f) * kPi / 180.0f;
        const bool major = (i % 5) == 0;
        const float outer = 218.0f;
        const float inner = outer - (major ? 24.0f : 10.0f);
        s_wf_tick_pts[i][0] = {static_cast<lv_value_precise_t>(kCenter + std::cos(angle) * inner),
                               static_cast<lv_value_precise_t>(kCenter + std::sin(angle) * inner)};
        s_wf_tick_pts[i][1] = {static_cast<lv_value_precise_t>(kCenter + std::cos(angle) * outer),
                               static_cast<lv_value_precise_t>(kCenter + std::sin(angle) * outer)};

        lv_obj_t *tick = lv_line_create(dial);
        lv_obj_set_pos(tick, 0, 0);
        lv_line_set_points(tick, s_wf_tick_pts[i], 2);
        lv_obj_set_style_line_width(tick, major ? 3 : 1, 0);
        lv_obj_set_style_line_color(tick, lv_color_hex(major ? 0xFFFFFF : 0x666666), 0);
        lv_obj_set_style_line_rounded(tick, true, 0);
    }

    auto make_hand = [&](lv_point_precise_t *points, int width, lv_color_t color) {
        lv_obj_t *hand = lv_line_create(dial);
        lv_obj_set_pos(hand, 0, 0);
        lv_line_set_points(hand, points, 2);
        lv_obj_set_style_line_width(hand, width, 0);
        lv_obj_set_style_line_color(hand, color, 0);
        lv_obj_set_style_line_rounded(hand, true, 0);
        return hand;
    };
    s_time_h = make_hand(s_wf_hour_pts, 9, lv_color_white());
    s_time_m = make_hand(s_wf_min_pts, 6, lv_color_white());

    lv_obj_t *cap = lv_obj_create(dial);
    lv_obj_set_size(cap, 14, 14);
    lv_obj_set_style_radius(cap, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(cap, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(cap, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(cap, 0, 0);
    lv_obj_align(cap, LV_ALIGN_CENTER, 0, 0);

    s_time_invalid = lv_label_create(page);
    lv_label_set_text(s_time_invalid, "时间未设置");
    lv_obj_set_style_text_font(s_time_invalid, &nex_cjk_20, 0);
    lv_obj_set_style_text_color(s_time_invalid, lv_color_white(), 0);
    lv_obj_align(s_time_invalid, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(s_time_invalid, LV_OBJ_FLAG_HIDDEN);
}

void build_menu()
{
    lv_obj_t *page = s_pages[NEX_PAGE_MENU];
    static const char *items[] = {"秒表", "姿态仪", "提醒事项", "计时器", "设置", "返回表盘"};
    for (int i = 0; i < 6; ++i) {
        s_menu_rows[i] = make_row(page, 64 + i * 56, NEX_ACT_SELECT, i, items[i], 20);
        lv_obj_add_event_cb(s_menu_rows[i], row_event_cb, LV_EVENT_CLICKED,
                            reinterpret_cast<void *>(static_cast<intptr_t>(NEX_ACT_SELECT * 1000 + i)));
    }
}

void timer_update_label();
void timer_reset();

void timer_start_stop()
{
    if (s_timer_alarm || s_timer_finished) {
        timer_reset();
        return;
    }
    audio_alarm_stop();
    s_timer_alarm = false;
    if (s_timer_running) {
        s_timer_remaining_ms = static_cast<int>(s_timer_end_ms - esp_timer_get_time() / 1000);
        if (s_timer_remaining_ms < 0) {
            s_timer_remaining_ms = 0;
        }
        s_timer_running = false;
    } else {
        if (s_timer_remaining_ms <= 0) {
            s_timer_remaining_ms = s_timer_duration_ms;
        }
        s_timer_end_ms = esp_timer_get_time() / 1000 + s_timer_remaining_ms;
        s_timer_running = true;
    }
    timer_update_label();
}

void timer_reset()
{
    audio_alarm_stop();
    s_timer_alarm = false;
    s_timer_finished = false;
    s_timer_running = false;
    s_timer_remaining_ms = s_timer_duration_ms;
    timer_update_label();
}

void timer_adjust(int delta_ms)
{
    int duration = s_timer_duration_ms + delta_ms;
    if (duration < 60000) duration = 60000;
    if (duration > 99 * 60000) duration = 99 * 60000;
    s_timer_duration_ms = duration;
    if (!s_timer_running) {
        s_timer_remaining_ms = duration;
    }
    timer_update_label();
}

void timer_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) {
        return;
    }
    const int action = static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(event)));
    if (action == 0) timer_adjust(-60000);
    else if (action == 1) timer_adjust(60000);
    else if (action == 2) timer_start_stop();
    else if (action == 3) timer_reset();
    else if (action == 4) {
        timer_adjust(10000);
    }
}

void timer_update_label()
{
    if (s_timer_value == nullptr) {
        return;
    }
    const int seconds = (s_timer_remaining_ms + 999) / 1000;
    char buffer[16];
    std::snprintf(buffer, sizeof(buffer), "%02d:%02d", seconds / 60, seconds % 60);
    set_text_if_changed(s_timer_value, buffer);
    const char *state = (s_timer_alarm || s_timer_finished) ? "时间到，按A键停止" : (s_timer_running ? "运行中" : (s_timer_remaining_ms == s_timer_duration_ms ? "待开始" : "已暂停"));
    set_text_if_changed(s_timer_state, state);
}

void build_timer()
{
    lv_obj_t *page = s_pages[NEX_PAGE_TIMER];

    s_timer_value = lv_label_create(page);
    lv_label_set_text(s_timer_value, "01:00");
    lv_obj_set_style_text_font(s_timer_value, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(s_timer_value, lv_color_white(), 0);
    lv_obj_align(s_timer_value, LV_ALIGN_CENTER, 0, -80);

    s_timer_state = lv_label_create(page);
    lv_label_set_text(s_timer_state, "待开始");
    lv_obj_set_style_text_font(s_timer_state, &nex_cjk_16, 0);
    lv_obj_set_style_text_color(s_timer_state, lv_color_hex(0x9A9AA2), 0);
    lv_obj_align(s_timer_state, LV_ALIGN_CENTER, 0, -25);

    auto add_button = [&](const char *text, int action, int x, int y, int width, int height) {
        lv_obj_t *button = lv_button_create(page);
        lv_obj_set_size(button, width, height);
        lv_obj_align(button, LV_ALIGN_CENTER, x, y);
        lv_obj_set_style_radius(button, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(button, lv_color_hex(0x141416), 0);
        lv_obj_set_style_border_width(button, 1, 0);
        lv_obj_set_style_border_color(button, lv_color_hex(0x3A3A40), 0);
        lv_obj_add_event_cb(button, timer_event_cb, LV_EVENT_CLICKED,
                            reinterpret_cast<void *>(static_cast<intptr_t>(action)));
        lv_obj_t *label = lv_label_create(button);
        lv_label_set_text(label, text);
        lv_obj_set_style_text_font(label, &nex_cjk_16, 0);
        lv_obj_set_style_text_color(label, lv_color_white(), 0);
        lv_obj_center(label);
    };
    add_button("+10秒", 4, -80, 30, 110, 46);
    add_button("+1分", 1, 80, 30, 110, 46);
    add_button("开始/暂停", 2, 0, 100, 200, 54);
    add_button("归零", 3, -65, 165, 110, 46);

    lv_obj_t *back = lv_button_create(page);
    lv_obj_set_size(back, 110, 46);
    lv_obj_align(back, LV_ALIGN_CENTER, 65, 165);
    lv_obj_set_style_radius(back, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(back, lv_color_hex(0x141416), 0);
    lv_obj_set_style_border_width(back, 1, 0);
    lv_obj_set_style_border_color(back, lv_color_hex(0x3A3A40), 0);
    lv_obj_add_event_cb(back, row_event_cb, LV_EVENT_CLICKED,
                        reinterpret_cast<void *>(static_cast<intptr_t>(NEX_ACT_BACK * 1000)));
    lv_obj_t *back_label = lv_label_create(back);
    lv_label_set_text(back_label, "返回");
    lv_obj_set_style_text_font(back_label, &nex_cjk_16, 0);
    lv_obj_set_style_text_color(back_label, lv_color_white(), 0);
    lv_obj_center(back_label);
}

void build_stopwatch()
{
    lv_obj_t *page = s_pages[NEX_PAGE_STOPWATCH];

    s_sw_time = lv_label_create(page);
    lv_label_set_text(s_sw_time, "00:00.0");
    lv_obj_set_style_text_font(s_sw_time, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(s_sw_time, lv_color_white(), 0);
    lv_obj_align(s_sw_time, LV_ALIGN_CENTER, 0, -60);

    s_sw_state = lv_label_create(page);
    lv_label_set_text(s_sw_state, "就绪");
    lv_obj_set_style_text_font(s_sw_state, &nex_cjk_16, 0);
    lv_obj_set_style_text_color(s_sw_state, lv_color_hex(0x9A9AA2), 0);
    lv_obj_align(s_sw_state, LV_ALIGN_CENTER, 0, 4);

    auto make_button = [&](const char *text, int action, int x_ofs) {
        lv_obj_t *button = lv_button_create(page);
        lv_obj_set_size(button, 132, 56);
        lv_obj_align(button, LV_ALIGN_BOTTOM_MID, x_ofs, -100);
        lv_obj_set_style_radius(button, 28, 0);
        lv_obj_set_style_bg_color(button, lv_color_hex(0x141414), 0);
        lv_obj_set_style_border_width(button, 1, 0);
        lv_obj_set_style_border_color(button, lv_color_hex(0x3A3A3A), 0);
        lv_obj_add_event_cb(button, row_event_cb, LV_EVENT_CLICKED,
                            reinterpret_cast<void *>(static_cast<intptr_t>(action * 1000)));
        lv_obj_t *label = lv_label_create(button);
        lv_label_set_text(label, text);
        lv_obj_set_style_text_font(label, &nex_cjk_16, 0);
        lv_obj_set_style_text_color(label, lv_color_white(), 0);
        lv_obj_center(label);
    };
    make_button("归零", NEX_ACT_STOPWATCH_RESET, -90);
    make_button("返回", NEX_ACT_BACK, 90);

    lv_obj_t *hint = lv_label_create(page);
    lv_label_set_text(hint, "B 开始   A 暂停");
    lv_obj_set_style_text_font(hint, &nex_cjk_16, 0);
    lv_obj_set_style_text_color(hint, lv_color_hex(0x777777), 0);
    lv_obj_align(hint, LV_ALIGN_CENTER, 0, 62);
}

void build_gyro()
{
    lv_obj_t *page = s_pages[NEX_PAGE_GYRO];

    lv_obj_t *ring = lv_obj_create(page);
    lv_obj_set_size(ring, 250, 250);
    lv_obj_align(ring, LV_ALIGN_CENTER, 0, -60);
    lv_obj_set_style_radius(ring, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(ring, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(ring, 2, 0);
    lv_obj_set_style_border_color(ring, lv_color_hex(0x555555), 0);
    lv_obj_set_style_pad_all(ring, 0, 0);
    lv_obj_remove_flag(ring, LV_OBJ_FLAG_SCROLLABLE);

    s_att_buf = static_cast<uint16_t *>(heap_caps_malloc(220 * 220 * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (s_att_buf != nullptr) {
        std::memset(s_att_buf, 0, 220 * 220 * 2);
        s_att_canvas = lv_canvas_create(ring);
        lv_canvas_set_buffer(s_att_canvas, s_att_buf, 220, 220, LV_COLOR_FORMAT_RGB565);
        lv_obj_center(s_att_canvas);
        lv_obj_set_style_border_width(s_att_canvas, 0, 0);
    }

    static lv_point_precise_t left_pts[2] = {{58, 125}, {105, 125}};
    static lv_point_precise_t right_pts[2] = {{145, 125}, {192, 125}};
    static lv_point_precise_t dot_pts[2] = {{119, 125}, {131, 125}};
    lv_point_precise_t *aircraft_pts[3] = {left_pts, right_pts, dot_pts};
    for (int i = 0; i < 3; ++i) {
        s_aircraft[i] = lv_line_create(ring);
        lv_obj_set_pos(s_aircraft[i], 0, 0);
        lv_line_set_points(s_aircraft[i], aircraft_pts[i], 2);
        lv_obj_set_style_line_width(s_aircraft[i], i == 2 ? 6 : 4, 0);
        lv_obj_set_style_line_color(s_aircraft[i], lv_color_hex(0xFFD24A), 0);
        lv_obj_set_style_line_rounded(s_aircraft[i], true, 0);
    }

    static const int bank_angles[9] = {-45, -30, -20, -10, 0, 10, 20, 30, 45};
    for (int i = 0; i < 9; ++i) {
        const float angle = static_cast<float>(bank_angles[i]) * 0.0174533f;
        const float inner = 100.0f;
        const float outer = 110.0f;
        s_bank_pts[i][0] = {static_cast<lv_value_precise_t>(125 + std::sin(angle) * inner),
                            static_cast<lv_value_precise_t>(125 - std::cos(angle) * inner)};
        s_bank_pts[i][1] = {static_cast<lv_value_precise_t>(125 + std::sin(angle) * outer),
                            static_cast<lv_value_precise_t>(125 - std::cos(angle) * outer)};
        lv_obj_t *tick = lv_line_create(ring);
        lv_obj_set_pos(tick, 0, 0);
        lv_line_set_points(tick, s_bank_pts[i], 2);
        lv_obj_set_style_line_width(tick, 2, 0);
        lv_obj_set_style_line_color(tick, lv_color_hex(0xFFFFFF), 0);
    }

    s_bank_marker = lv_line_create(ring);
    lv_obj_set_pos(s_bank_marker, 0, 0);
    lv_line_set_points(s_bank_marker, s_bank_marker_pts, 2);
    lv_obj_set_style_line_width(s_bank_marker, 3, 0);
    lv_obj_set_style_line_color(s_bank_marker, lv_color_hex(0xFFD24A), 0);

    lv_obj_t *back = lv_button_create(page);
    lv_obj_set_size(back, 150, 50);
    lv_obj_align(back, LV_ALIGN_BOTTOM_MID, 0, -20);
    lv_obj_set_style_radius(back, 25, 0);
    lv_obj_set_style_bg_color(back, lv_color_hex(0x141414), 0);
    lv_obj_set_style_border_width(back, 1, 0);
    lv_obj_set_style_border_color(back, lv_color_hex(0x3A3A3A), 0);
    lv_obj_add_event_cb(back, row_event_cb, LV_EVENT_CLICKED,
                        reinterpret_cast<void *>(static_cast<intptr_t>(NEX_ACT_BACK * 1000)));
    lv_obj_t *label = lv_label_create(back);
    lv_label_set_text(label, "返回");
    lv_obj_set_style_text_font(label, &nex_cjk_16, 0);
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    lv_obj_center(label);
}

void build_phone()
{
    lv_obj_t *page = s_pages[NEX_PAGE_PHONE];

    lv_obj_t *title = lv_label_create(page);
    lv_label_set_text(title, "蓝牙: NEX-Watch");
    lv_obj_set_style_text_font(title, &nex_cjk_16, 0);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, -70);

    lv_obj_t *hint = lv_label_create(page);
    lv_label_set_text(hint, "打开 NEX_Connect 连接设备");
    lv_obj_set_width(hint, 420);
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(hint, &nex_cjk_16, 0);
    lv_obj_set_style_text_color(hint, lv_color_hex(0x9A9AA2), 0);
    lv_obj_align(hint, LV_ALIGN_CENTER, 0, 20);

    s_phone_status = lv_label_create(page);
    lv_label_set_text(s_phone_status, "等待手机连接");
    lv_obj_set_style_text_font(s_phone_status, &nex_cjk_16, 0);
    lv_obj_set_style_text_color(s_phone_status, lv_color_hex(0xFFB020), 0);
    lv_obj_align(s_phone_status, LV_ALIGN_CENTER, 0, 70);

    lv_obj_t *back = lv_button_create(page);
    lv_obj_set_size(back, 170, 54);
    lv_obj_align(back, LV_ALIGN_BOTTOM_MID, 0, -26);
    lv_obj_set_style_radius(back, 27, 0);
    lv_obj_set_style_bg_color(back, lv_color_hex(0x141414), 0);
    lv_obj_set_style_border_width(back, 1, 0);
    lv_obj_set_style_border_color(back, lv_color_hex(0x3A3A3A), 0);
    lv_obj_add_event_cb(back, row_event_cb, LV_EVENT_CLICKED,
                        reinterpret_cast<void *>(static_cast<intptr_t>(NEX_ACT_BACK * 1000)));
    lv_obj_t *label = lv_label_create(back);
    lv_label_set_text(label, "停止并返回");
    lv_obj_set_style_text_font(label, &nex_cjk_16, 0);
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    lv_obj_center(label);
}

void refresh_reminders();

void reminder_row_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) {
        return;
    }
    const int index = static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(event)));
    reminders_toggle(index);
    refresh_reminders();
}

void refresh_reminders()
{
    if (s_reminder_list == nullptr) {
        return;
    }
    const int count = reminders_count();
    const int selection = nex_core_selection();
    const int generation = reminders_generation();
    if (count == s_reminder_count && selection == s_reminder_sel && generation == s_reminder_gen) {
        return;
    }
    s_reminder_count = count;
    s_reminder_sel = selection;
    s_reminder_gen = generation;

    lv_obj_clean(s_reminder_list);
    lv_obj_t *selected_row = nullptr;
    for (int i = 0; i < count; ++i) {
        const Reminder *item = reminders_get(i);
        if (item == nullptr) {
            break;
        }
        lv_obj_t *row = lv_obj_create(s_reminder_list);
        lv_obj_set_size(row, 400, 64);
        lv_obj_set_pos(row, 10, i * 72);
        lv_obj_set_style_radius(row, 32, 0);
        lv_obj_set_style_bg_color(row, lv_color_hex(0x141416), 0);
        lv_obj_set_style_border_width(row, i == selection ? 2 : 1, 0);
        lv_obj_set_style_border_color(row, i == selection ? lv_color_white() : lv_color_hex(0x3A3A40), 0);
        lv_obj_add_event_cb(row, reminder_row_cb, LV_EVENT_CLICKED,
                            reinterpret_cast<void *>(static_cast<intptr_t>(i)));

        lv_obj_t *dot = lv_obj_create(row);
        lv_obj_set_size(dot, 20, 20);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(dot, 2, 0);
        lv_obj_set_style_border_color(dot, lv_color_white(), 0);
        lv_obj_set_style_bg_color(dot, lv_color_hex(0xFFD24A), 0);
        lv_obj_set_style_bg_opa(dot, item->done ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        lv_obj_align(dot, LV_ALIGN_LEFT_MID, 16, 0);

        lv_obj_t *label = lv_label_create(row);
        lv_label_set_text(label, item->text);
        lv_obj_set_width(label, 320);
        lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_font(label, &nex_cjk_text_16, 0);
        lv_obj_set_style_text_color(label, item->done ? lv_color_hex(0x808080) : lv_color_white(), 0);
        lv_obj_align(label, LV_ALIGN_LEFT_MID, 52, 0);

        if (i == selection) {
            selected_row = row;
        }
    }
    if (selected_row != nullptr) {
        lv_obj_scroll_to_view(selected_row, LV_ANIM_ON);
    }
    if (s_reminder_empty != nullptr) {
        if (count == 0) {
            lv_obj_remove_flag(s_reminder_empty, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_reminder_empty, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

void build_reminders()
{
    lv_obj_t *page = s_pages[NEX_PAGE_REMINDERS];
    s_reminder_list = lv_obj_create(page);
    lv_obj_set_size(s_reminder_list, 420, 300);
    lv_obj_align(s_reminder_list, LV_ALIGN_TOP_MID, 0, 96);
    lv_obj_set_style_bg_opa(s_reminder_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_reminder_list, 0, 0);
    lv_obj_set_style_pad_all(s_reminder_list, 0, 0);
    lv_obj_set_scroll_dir(s_reminder_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_reminder_list, LV_SCROLLBAR_MODE_OFF);

    s_reminder_empty = lv_label_create(page);
    lv_label_set_text(s_reminder_empty, "暂无提醒，手机添加");
    lv_obj_set_width(s_reminder_empty, 420);
    lv_obj_set_style_text_align(s_reminder_empty, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(s_reminder_empty, &nex_cjk_text_16, 0);
    lv_obj_set_style_text_color(s_reminder_empty, lv_color_hex(0x808080), 0);
    lv_obj_align(s_reminder_empty, LV_ALIGN_CENTER, 0, 0);
}

void build_settings()
{
    lv_obj_t *page = s_pages[NEX_PAGE_SETTINGS];
    static const char *items[] = {"时间与日期", "系统信息", "开发者选项", "手机连接", "返回表盘"};
    for (int i = 0; i < 5; ++i) {
        s_settings_rows[i] = make_row(page, 88 + i * 60, NEX_ACT_SELECT, i, items[i], 20);
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
        lv_obj_set_style_text_font(label, &nex_cjk_20, 0);
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
        lv_obj_remove_flag(s_time_invalid, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    char hour_minute[5] = {digits[0], digits[1], digits[2], digits[3], 0};
    static char last_hour_minute[5] = "";
    if (std::strcmp(hour_minute, last_hour_minute) == 0) {
        return;
    }
    std::snprintf(last_hour_minute, sizeof(last_hour_minute), "%s", hour_minute);

    lv_obj_remove_flag(s_time_h, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s_time_m, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_time_invalid, LV_OBJ_FLAG_HIDDEN);

    const float hour = (digits[0] - '0') * 10 + (digits[1] - '0');
    const float minute = (digits[2] - '0') * 10 + (digits[3] - '0');
    constexpr float kPi = 3.14159265f;
    constexpr float kCenter = 230.0f;

    auto set_hand = [&](lv_point_precise_t *points, lv_obj_t *hand, float degrees, float length) {
        const float angle = (degrees - 90.0f) * kPi / 180.0f;
        points[0].x = static_cast<lv_value_precise_t>(kCenter);
        points[0].y = static_cast<lv_value_precise_t>(kCenter);
        points[1].x = static_cast<lv_value_precise_t>(kCenter + std::cos(angle) * length);
        points[1].y = static_cast<lv_value_precise_t>(kCenter + std::sin(angle) * length);
        lv_line_set_points(hand, points, 2);
    };
    set_hand(s_wf_hour_pts, s_time_h, (hour + minute / 60.0f) * 30.0f, 105.0f);
    set_hand(s_wf_min_pts, s_time_m, minute * 6.0f, 150.0f);
}

void refresh_menu()
{
    const int sel = nex_core_selection();
    for (int i = 0; i < 6; ++i) {
        style_row_selected(s_menu_rows[i], i == sel);
    }
}

void refresh_gyro()
{
    float ax = 0, ay = 0, az = 0, gx = 0, gy = 0, gz = 0;
    if (!imu_read(ax, ay, az, gx, gy, gz)) {
        return;
    }

    const float screen_x = -ay;
    const float screen_y = -ax;
    const float roll = std::atan2(screen_x, az) * 57.2958f;
    const float pitch = std::atan2(screen_y, std::sqrt(screen_x * screen_x + az * az)) * 57.2958f;

    const int theme = nex_core_theme();
    if (theme != s_att_theme) {
        s_att_theme = theme;
        const AttPalette palette = palette_for(theme);
        for (auto *line : s_aircraft) {
            lv_obj_set_style_line_color(line, lv_color_hex(palette.aircraft), 0);
        }
        if (s_bank_marker != nullptr) {
            lv_obj_set_style_line_color(s_bank_marker, lv_color_hex(palette.bank), 0);
        }
    }

    draw_attitude(roll, pitch, theme);

    const float angle = roll * 0.0174533f;
    s_bank_marker_pts[0].x = static_cast<lv_value_precise_t>(125 + std::sin(angle) * 92.0f);
    s_bank_marker_pts[0].y = static_cast<lv_value_precise_t>(125 - std::cos(angle) * 92.0f);
    s_bank_marker_pts[1].x = static_cast<lv_value_precise_t>(125 + std::sin(angle) * 106.0f);
    s_bank_marker_pts[1].y = static_cast<lv_value_precise_t>(125 - std::cos(angle) * 106.0f);
    lv_line_set_points(s_bank_marker, s_bank_marker_pts, 2);
}

void refresh_timer()
{
    const int64_t now = esp_timer_get_time() / 1000;
    if (s_timer_running) {
        s_timer_remaining_ms = static_cast<int>(s_timer_end_ms - now);
        if (s_timer_remaining_ms <= 0) {
            s_timer_remaining_ms = 0;
            s_timer_running = false;
            s_timer_finished = true;
            s_timer_alarm = true;
            s_timer_alarm_deadline = now + 10000;
            audio_alarm_start();
        }
        timer_update_label();
    }
    if (s_timer_alarm && now >= s_timer_alarm_deadline) {
        audio_alarm_stop();
        s_timer_alarm = false;
        timer_update_label();
    }
}

void refresh_stopwatch()
{
    static uint64_t last_tenth = ~0ull;
    const uint64_t ms = nex_core_stopwatch_ms();
    const uint64_t tenths = ms / 100;
    if (tenths == last_tenth) {
        return;
    }
    last_tenth = tenths;

    const uint64_t hours = tenths / 36000;
    const uint64_t minutes = (tenths / 600) % 60;
    const uint64_t seconds = (tenths / 10) % 60;
    const uint64_t fraction = tenths % 10;
    char buffer[32];
    if (hours > 0) {
        std::snprintf(buffer, sizeof(buffer), "%llu:%02llu:%02llu.%llu",
                      static_cast<unsigned long long>(hours), static_cast<unsigned long long>(minutes),
                      static_cast<unsigned long long>(seconds), static_cast<unsigned long long>(fraction));
    } else {
        std::snprintf(buffer, sizeof(buffer), "%02llu:%02llu.%llu", static_cast<unsigned long long>(minutes),
                      static_cast<unsigned long long>(seconds), static_cast<unsigned long long>(fraction));
    }
    set_text_if_changed(s_sw_time, buffer);
    const char *state = nex_core_stopwatch_running() ? "运行中" : (ms == 0 ? "就绪" : "已暂停");
    set_text_if_changed(s_sw_state, state);
}

void refresh_phone()
{
    if (s_phone_status == nullptr) {
        return;
    }
    if (!ble_upload_connected()) {
        set_text_if_changed(s_phone_status, "等待手机连接");
        return;
    }
    const int status = ble_upload_status();
    const int progress = ble_upload_progress();
    char text[48];
    if (status == 2) {
        std::snprintf(text, sizeof(text), "壁纸已更新");
    } else if (status == 3) {
        std::snprintf(text, sizeof(text), "上传失败");
    } else if (progress >= 0) {
        std::snprintf(text, sizeof(text), "接收中 %d%%", progress);
    } else {
        std::snprintf(text, sizeof(text), "已连接，等待图片");
    }
    set_text_if_changed(s_phone_status, text);
}

void refresh_settings()
{
    const int sel = nex_core_selection();
    for (int i = 0; i < 5; ++i) {
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
        case NEX_PAGE_STOPWATCH:
            refresh_stopwatch();
            break;
        case NEX_PAGE_MENU:
            refresh_menu();
            break;
        case NEX_PAGE_GYRO:
            refresh_gyro();
            break;
        case NEX_PAGE_PHONE:
            refresh_phone();
            break;
        case NEX_PAGE_REMINDERS:
            refresh_reminders();
            break;
        case NEX_PAGE_TIMER:
            refresh_timer();
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
    if (s_current_page == NEX_PAGE_PHONE && page != NEX_PAGE_PHONE) {
        ble_upload_advertise(false);
    }
    if (s_current_page == NEX_PAGE_TIMER && page != NEX_PAGE_TIMER) {
        audio_alarm_stop();
        s_timer_alarm = false;
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
    if (page == NEX_PAGE_PHONE) {
        ble_upload_advertise(true);
    }
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
            if (s_current_page == NEX_PAGE_TIMER && s_timer_alarm &&
                (event == NEX_KEY_A_CLICK || event == NEX_KEY_B_CLICK)) {
                timer_reset();
                dirty = true;
                continue;
            }
            if (s_current_page == NEX_PAGE_TIMER && event == NEX_KEY_A_CLICK) {
                timer_reset();
                dirty = true;
                continue;
            }
            if (s_current_page == NEX_PAGE_TIMER && event == NEX_KEY_B_CLICK) {
                timer_start_stop();
                dirty = true;
                continue;
            }
            if (event == NEX_KEY_BOTH_CLICK && s_current_page == NEX_PAGE_REMINDERS) {
                reminders_toggle(nex_core_selection());
                refresh_reminders();
                dirty = true;
                continue;
            }
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
            if (s_current_page == NEX_PAGE_STOPWATCH) {
                refresh_stopwatch();
            }
            if (s_current_page == NEX_PAGE_GYRO) {
                refresh_gyro();
            }
            if (s_current_page == NEX_PAGE_PHONE) {
                refresh_phone();
            }
            if (s_current_page == NEX_PAGE_TIMER) {
                refresh_timer();
            }
            {
                const int generation = wallpaper_generation();
                if (generation != s_wall_gen && s_wallpaper != nullptr) {
                    s_wall_gen = generation;
                    if (wallpaper_exists()) {
                        lv_image_cache_drop(wallpaper_lvgl_path());
                        lv_image_set_src(s_wallpaper, wallpaper_lvgl_path());
                        lv_obj_align(s_wallpaper, LV_ALIGN_CENTER, 0, 0);
                        lv_obj_remove_flag(s_wallpaper, LV_OBJ_FLAG_HIDDEN);
                    } else {
                        lv_obj_add_flag(s_wallpaper, LV_OBJ_FLAG_HIDDEN);
                    }
                }
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
    build_menu();
    build_stopwatch();
    build_gyro();
    build_phone();
    build_reminders();
    build_timer();
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
