/*
 * NEX firmware — physical keys (KEYA / KEYB)
 * SPDX-License-Identifier: MIT
 *
 * KEYA = GPIO2, KEYB = GPIO1, both active low with internal pull-ups
 * (M5StopWatch-UserDemo 0.5, main/hal/hal_button.cpp).
 *
 * A dedicated task polls every 10 ms. A key changes its debounced state only
 * after the raw level has been stable for 25 ms. Long press is measured from
 * the monotonic esp_timer clock, never by blocking.
 *
 * Event rules:
 *   - both keys stable-down -> BOTH PRESSED (single keys are suppressed)
 *   - single key stable-down -> KEYA/KEYB PRESSED
 *   - 1000 ms stable hold    -> LONG PRESS, emitted once per press cycle
 *   - after a combo ends, a key that is still held is "used in combo" and
 *     produces no single-key press/long event until it is released
 *
 * The task never touches LVGL: status text is posted to the UI task through a
 * queue and applied there under the LVGL mutex.
 */
#include "keys.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ui.h"

namespace nex {
namespace {

constexpr const char *TAG = "NEX_KEY";

constexpr gpio_num_t kKeyA = GPIO_NUM_2;
constexpr gpio_num_t kKeyB = GPIO_NUM_1;

constexpr TickType_t kPollTicks = pdMS_TO_TICKS(10);
constexpr int64_t kDebounceMs = 25;
constexpr int64_t kLongPressMs = 1000;

struct KeyState {
    bool raw_down = false;
    int64_t raw_since_ms = 0;
    bool stable_down = false;
    int64_t down_since_ms = 0;
    bool long_fired = false;
    bool used_in_combo = false;
};

KeyState s_key_a;
KeyState s_key_b;

bool s_combo_active = false;
int64_t s_combo_since_ms = 0;
bool s_combo_long_fired = false;

int64_t now_ms()
{
    return esp_timer_get_time() / 1000;
}

void emit(const char *log_text, const char *status_text)
{
    ESP_LOGI(TAG, "%s", log_text);
    ui_post_status(status_text);
}

// Returns true when the debounced state changed on this sample.
bool update_key(KeyState &key, bool raw_down, int64_t now)
{
    if (raw_down != key.raw_down) {
        key.raw_down = raw_down;
        key.raw_since_ms = now;
    }

    if (key.raw_down == key.stable_down || now - key.raw_since_ms < kDebounceMs) {
        return false;
    }

    key.stable_down = key.raw_down;
    if (key.stable_down) {
        key.down_since_ms = now;
        key.long_fired = false;
        key.used_in_combo = false;
    }
    return true;
}

void handle_press_events(bool a_changed, bool b_changed, int64_t now)
{
    const bool both = s_key_a.stable_down && s_key_b.stable_down;

    if (both && !s_combo_active) {
        s_combo_active = true;
        s_combo_since_ms = now;
        s_combo_long_fired = false;
        // From now on neither key may generate a single-key event.
        s_key_a.used_in_combo = true;
        s_key_b.used_in_combo = true;
        emit("BOTH PRESSED", "KEYA + KEYB: PRESSED");
        return;
    }

    if (!both && s_combo_active) {
        s_combo_active = false;
        s_combo_long_fired = false;
        // The key still held after the combo must not turn into a single press.
        if (s_key_a.stable_down) {
            s_key_a.used_in_combo = true;
        }
        if (s_key_b.stable_down) {
            s_key_b.used_in_combo = true;
        }
    }

    if (s_combo_active) {
        return;
    }
    if (a_changed && s_key_a.stable_down && !s_key_b.stable_down && !s_key_a.used_in_combo) {
        emit("KEYA PRESSED", "KEYA: PRESSED");
    }
    if (b_changed && s_key_b.stable_down && !s_key_a.stable_down && !s_key_b.used_in_combo) {
        emit("KEYB PRESSED", "KEYB: PRESSED");
    }
}

void handle_long_press(int64_t now)
{
    const bool both = s_key_a.stable_down && s_key_b.stable_down;

    if (s_combo_active && both) {
        if (!s_combo_long_fired && now - s_combo_since_ms >= kLongPressMs) {
            s_combo_long_fired = true;
            emit("BOTH LONG PRESS (1000ms)", "KEYA + KEYB: LONG PRESS (1s)");
        }
        return;
    }

    if (s_combo_active) {
        return;
    }

    if (s_key_a.stable_down && !s_key_b.stable_down && !s_key_a.used_in_combo && !s_key_a.long_fired &&
        now - s_key_a.down_since_ms >= kLongPressMs) {
        s_key_a.long_fired = true;
        emit("KEYA LONG PRESS (1000ms)", "KEYA: LONG PRESS (1s)");
    }
    if (s_key_b.stable_down && !s_key_a.stable_down && !s_key_b.used_in_combo && !s_key_b.long_fired &&
        now - s_key_b.down_since_ms >= kLongPressMs) {
        s_key_b.long_fired = true;
        emit("KEYB LONG PRESS (1000ms)", "KEYB: LONG PRESS (1s)");
    }
}

void key_task(void *arg)
{
    (void)arg;
    TickType_t last_wake = xTaskGetTickCount();

    while (true) {
        const int64_t now = now_ms();
        const bool a_changed = update_key(s_key_a, gpio_get_level(kKeyA) == 0, now);
        const bool b_changed = update_key(s_key_b, gpio_get_level(kKeyB) == 0, now);

        handle_press_events(a_changed, b_changed, now);
        handle_long_press(now);

        vTaskDelayUntil(&last_wake, kPollTicks);
    }
}

}  // namespace

bool keys_start()
{
    gpio_config_t cfg = {};
    cfg.pin_bit_mask = (1ULL << kKeyA) | (1ULL << kKeyB);
    cfg.mode = GPIO_MODE_INPUT;
    cfg.pull_up_en = GPIO_PULLUP_ENABLE;
    cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    cfg.intr_type = GPIO_INTR_DISABLE;

    const esp_err_t err = gpio_config(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "GPIO config failed: %s", esp_err_to_name(err));
        return false;
    }

    vTaskDelay(pdMS_TO_TICKS(10));
    ESP_LOGI(TAG, "KEYA=GPIO%d KEYB=GPIO%d, idle level A=%d B=%d (1=released)", (int)kKeyA, (int)kKeyB,
             gpio_get_level(kKeyA), gpio_get_level(kKeyB));

    if (xTaskCreate(key_task, "nex_keys", 3 * 1024, nullptr, 4, nullptr) != pdPASS) {
        ESP_LOGE(TAG, "key task creation failed");
        return false;
    }
    return true;
}

}  // namespace nex
