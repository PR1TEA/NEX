/*
 * NEX system core — C ABI boundary between Rust and C++
 * SPDX-License-Identifier: MIT
 *
 * Rust owns: clock policy, settings values, navigation, app registry and
 * compatibility checks.
 * C++ owns: board access, RX8130CE I2C, NVS, LVGL objects and the UI task.
 *
 * Only fixed-width integers, small C structs of plain integers/char arrays and
 * borrowed NUL-terminated strings cross this boundary.
 */
#pragma once

#include <stdint.h>

#define NEX_CORE_API_VERSION 1

typedef enum {
    NEX_PAGE_WATCHFACE = 0,
    NEX_PAGE_SETTINGS = 1,
    NEX_PAGE_TIME_EDITOR = 2,
    NEX_PAGE_SYSTEM_INFO = 3,
    NEX_PAGE_DEVELOPER = 4,
    NEX_PAGE_INPUT_DIAG = 5,
    NEX_PAGE_APPS = 6,
    NEX_PAGE_APP_RUNNER = 7,
    NEX_PAGE_STOPWATCH = 8,
    NEX_PAGE_MENU = 9,
    NEX_PAGE_GYRO = 10,
    NEX_PAGE_PHONE = 11,
    NEX_PAGE_REMINDERS = 12,
    NEX_PAGE_TIMER = 13,
} nex_page_t;

typedef enum {
    NEX_KEY_A_CLICK = 1,
    NEX_KEY_B_CLICK = 2,
    NEX_KEY_A_LONG = 3,
    NEX_KEY_B_LONG = 4,
    NEX_KEY_BOTH_LONG = 5,
    NEX_KEY_BOTH_CLICK = 6,
} nex_key_event_t;

typedef enum {
    NEX_ACT_UP = 1,
    NEX_ACT_DOWN = 2,
    NEX_ACT_CONFIRM = 3,
    NEX_ACT_BACK = 4,
    NEX_ACT_SELECT = 5, /* arg = item index */
    NEX_ACT_TOGGLE_DEV = 6,
    NEX_ACT_HOME = 7,
    NEX_ACT_STOPWATCH_TOGGLE = 8,
    NEX_ACT_STOPWATCH_RESET = 9,
    NEX_ACT_OPEN_STOPWATCH = 10,
    NEX_ACT_OPEN_MENU = 11,
    NEX_ACT_STOPWATCH_START = 12,
    NEX_ACT_STOPWATCH_PAUSE = 13,
    NEX_ACT_THEME_PREV = 14,
    NEX_ACT_THEME_NEXT = 15,
} nex_action_t;

typedef enum {
    NEX_CLOCK_INVALID = 0,
    NEX_CLOCK_MANUAL = 1,
    NEX_CLOCK_RTC = 2,
    NEX_CLOCK_BLUETOOTH = 3,
    NEX_CLOCK_WIFI = 4,
} nex_clock_source_t;

typedef struct {
    int32_t year;
    int32_t month;
    int32_t day;
    int32_t hour;
    int32_t minute;
    int32_t second;
    int32_t tz_minutes;
} nex_civil_t;

typedef struct {
    char id[24];
    char name[40];
    char entry[24];
    uint32_t version;
    uint32_t required_api;
    int32_t compatible;
    char reason[64];
} nex_app_info_t;

/* Flags returned by nex_rtc_read(). */
#define NEX_RTC_FLAG_VLF       (1u << 0) /* oscillator stopped / data lost */
#define NEX_RTC_FLAG_STOPPED   (1u << 1) /* STOP bit set */
#define NEX_RTC_FLAG_MAGIC_OK  (1u << 2) /* RTC RAM says "written by NEX (UTC)" */
#define NEX_RTC_FLAG_I2C_OK    (1u << 3) /* all I2C transactions succeeded */

typedef struct {
    int32_t year;
    int32_t month;
    int32_t day;
    int32_t hour;
    int32_t minute;
    int32_t second;
    uint32_t flags;
} nex_rtc_fields_t;

#ifdef __cplusplus
extern "C" {
#endif

/* ---- implemented in Rust (components/nex_rust) ------------------------- */
int32_t nex_core_init(void);
void nex_core_tick(uint64_t mono_ms);
void nex_core_key_event(int32_t event);
void nex_core_action(int32_t action, int32_t arg);
int32_t nex_core_page(void);
int32_t nex_core_selection(void);
int32_t nex_core_dev_enabled(void);
int32_t nex_core_theme(void);
int32_t nex_core_clock_valid(void);
int32_t nex_core_clock_source(void);
int32_t nex_core_api_version(void);
void nex_core_time_digits(char *out); /* out: 7 bytes, "132030" or "------" */
void nex_core_local_fields(nex_civil_t *out);
int32_t nex_core_set_time(int32_t year, int32_t month, int32_t day, int32_t hour,
                          int32_t minute, int32_t second, int32_t tz_minutes);
/* Unified future time-source entry point. HardwareRtc/Manual are handled by
 * set_time(); Bluetooth/WiFi are reserved and return -1 (not implemented). */
int32_t nex_core_sync_time(int32_t source);
uint64_t nex_core_stopwatch_ms(void);
int32_t nex_core_stopwatch_running(void);
int32_t nex_core_app_count(void);
int32_t nex_core_app_info(int32_t index, nex_app_info_t *out);
int32_t nex_core_start_app(int32_t index);
int32_t nex_core_running_app(void);
void nex_core_last_error(char *out, int32_t len);

/* ---- implemented in C++ (main/services.cpp), called from Rust ---------- */
int32_t nex_rtc_read(nex_rtc_fields_t *out);
int32_t nex_rtc_write(int32_t year, int32_t month, int32_t day, int32_t hour,
                      int32_t minute, int32_t second);
int32_t nex_nvs_load(uint8_t *buf, int32_t len);
int32_t nex_nvs_save(const uint8_t *buf, int32_t len);
uint64_t nex_monotonic_ms(void);
void nex_log_from_rust(const char *message);

#ifdef __cplusplus
}
#endif
