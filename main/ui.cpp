/*
 * NEX firmware — Hello World UI
 * SPDX-License-Identifier: MIT
 *
 * LVGL 9.5 with two partial draw buffers in PSRAM. LVGL runs in its own task
 * and is only touched while holding s_lvgl_mutex; the touch callback runs
 * inside lv_timer_handler() in that same task.
 */
#include "ui.h"

#include <cstdio>

#include "board.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lvgl.h"

namespace nex {
namespace {

constexpr const char *TAG = "NEX-UI";

// Panel window used by the STOPWATCH firmware (M5GFX Panel_AMOLED config).
constexpr uint32_t kDispW = 468;
constexpr uint32_t kDispH = 466;
constexpr uint32_t kBufLines = 40;

SemaphoreHandle_t s_lvgl_mutex = nullptr;
lv_obj_t *s_status_label = nullptr;
bool s_touch_ok = false;

// Status strings produced by other tasks (e.g. the key scanner) are queued
// here and applied by the UI task, never from the producer's context.
struct StatusMessage {
    char text[48];
};
QueueHandle_t s_status_queue = nullptr;

void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    auto *gfx = static_cast<lgfx::LGFX_Device *>(lv_display_get_user_data(disp));

    const int32_t w = area->x2 - area->x1 + 1;
    const int32_t h = area->y2 - area->y1 + 1;
    const uint32_t pixels = static_cast<uint32_t>(w) * static_cast<uint32_t>(h);

    gfx->startWrite();
    gfx->setAddrWindow(area->x1, area->y1, w, h);

    // One contiguous write per window. The active panel is the AMOLED
    // framebuffer, so this copies into PSRAM and M5GFX pushes the dirty range
    // to the panel through its own internal DMA buffers on endWrite().
    const auto *src = reinterpret_cast<const lgfx::rgb565_t *>(px_map);
    gfx->writePixels(src, pixels);

    gfx->endWrite();
    lv_display_flush_ready(disp);
}

void touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;

    uint16_t x = 0;
    uint16_t y = 0;
    bool pressed = false;
    if (!s_touch_ok || !board_touch_read(x, y, pressed)) {
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
}

void tick_cb(void *arg)
{
    (void)arg;
    lv_tick_inc(2);
}

void ui_task(void *arg)
{
    (void)arg;
    while (true) {
        if (xSemaphoreTake(s_lvgl_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            StatusMessage msg;
            while (xQueueReceive(s_status_queue, &msg, 0) == pdTRUE) {
                if (s_status_label != nullptr) {
                    lv_label_set_text(s_status_label, msg.text);
                }
            }
            lv_timer_handler();
            xSemaphoreGive(s_lvgl_mutex);
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

void button_event_cb(lv_event_t *event)
{
    const char *name = static_cast<const char *>(lv_event_get_user_data(event));
    char text[48];

    switch (lv_event_get_code(event)) {
        case LV_EVENT_PRESSED:
            std::snprintf(text, sizeof(text), "%s pressed", name);
            ESP_LOGI(TAG, "touch test: %s pressed", name);
            break;
        case LV_EVENT_RELEASED:
            std::snprintf(text, sizeof(text), "%s released", name);
            ESP_LOGI(TAG, "touch test: %s released", name);
            break;
        case LV_EVENT_PRESS_LOST:
            std::snprintf(text, sizeof(text), "%s released", name);
            break;
        default:
            return;
    }

    if (s_status_label != nullptr) {
        lv_label_set_text(s_status_label, text);
    }
}

lv_obj_t *make_button(lv_obj_t *parent, const char *name, int32_t y_offset)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_size(btn, 200, 54);
    lv_obj_align(btn, LV_ALIGN_CENTER, 0, y_offset);
    lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x1F1F24), 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x0A84FF), LV_STATE_PRESSED);
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_set_style_border_color(btn, lv_color_hex(0x3A3A40), 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_add_event_cb(btn, button_event_cb, LV_EVENT_PRESSED, const_cast<char *>(name));
    lv_obj_add_event_cb(btn, button_event_cb, LV_EVENT_RELEASED, const_cast<char *>(name));
    lv_obj_add_event_cb(btn, button_event_cb, LV_EVENT_PRESS_LOST, const_cast<char *>(name));

    lv_obj_t *label = lv_label_create(btn);
    lv_label_set_text(label, name);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    lv_obj_center(label);

    return btn;
}

void build_ui()
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "NEX");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 34);

    lv_obj_t *hello = lv_label_create(scr);
    lv_label_set_text(hello, "Hello World!");
    lv_obj_set_style_text_font(hello, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(hello, lv_color_hex(0xE8E8EC), 0);
    lv_obj_align(hello, LV_ALIGN_CENTER, 0, -104);

    make_button(scr, "Button A", -18);
    make_button(scr, "Button B", 52);
    make_button(scr, "Button C", 122);

    s_status_label = lv_label_create(scr);
    lv_label_set_text(s_status_label, s_touch_ok ? "Ready" : "Touch init failed");
    lv_obj_set_style_text_font(s_status_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_status_label, lv_color_hex(0x9A9AA2), 0);
    lv_obj_align(s_status_label, LV_ALIGN_BOTTOM_MID, 0, -34);
}

}  // namespace

bool ui_start()
{
    lv_init();

    lv_display_t *disp = lv_display_create(kDispW, kDispH);
    if (disp == nullptr) {
        ESP_LOGE(TAG, "lv_display_create failed");
        return false;
    }

    // RGB565: two bytes per pixel. sizeof(lv_color_t) is 3 in LVGL 9 (the
    // API colour type), which is NOT the draw-buffer pixel size.
    constexpr size_t kBytesPerPixel = 2;
    const size_t buf_bytes = static_cast<size_t>(kDispW) * kBufLines * kBytesPerPixel;
    auto *buf1 = static_cast<uint8_t *>(heap_caps_malloc(buf_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    auto *buf2 = static_cast<uint8_t *>(heap_caps_malloc(buf_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (buf1 == nullptr || buf2 == nullptr) {
        ESP_LOGE(TAG, "draw buffer allocation failed (%u bytes x2 in PSRAM)", (unsigned)buf_bytes);
        heap_caps_free(buf1);
        heap_caps_free(buf2);
        return false;
    }
    ESP_LOGI(TAG, "draw buffers: 2 x %u bytes (%u lines) in PSRAM, PSRAM free=%u",
             (unsigned)buf_bytes, (unsigned)kBufLines,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_user_data(disp, &board_display());
    lv_display_set_flush_cb(disp, flush_cb);
    lv_display_set_buffers(disp, buf1, buf2, buf_bytes, LV_DISPLAY_RENDER_MODE_PARTIAL);

    lv_indev_t *indev = lv_indev_create();
    if (indev == nullptr) {
        ESP_LOGE(TAG, "lv_indev_create failed");
        return false;
    }
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, touch_read_cb);
    lv_indev_set_display(indev, disp);

    s_touch_ok = board_touch_init();

    const esp_timer_create_args_t timer_args = {
        .callback = tick_cb,
        .arg = nullptr,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "lvgl_tick",
        .skip_unhandled_events = false,
    };
    esp_timer_handle_t tick_timer = nullptr;
    if (esp_timer_create(&timer_args, &tick_timer) != ESP_OK ||
        esp_timer_start_periodic(tick_timer, 2 * 1000) != ESP_OK) {
        ESP_LOGE(TAG, "LVGL tick timer setup failed");
        return false;
    }

    s_lvgl_mutex = xSemaphoreCreateMutex();
    if (s_lvgl_mutex == nullptr) {
        ESP_LOGE(TAG, "LVGL mutex creation failed");
        return false;
    }

    s_status_queue = xQueueCreate(8, sizeof(StatusMessage));
    if (s_status_queue == nullptr) {
        ESP_LOGE(TAG, "status queue creation failed");
        return false;
    }

    build_ui();

    if (xTaskCreate(ui_task, "nex_ui", 8 * 1024, nullptr, 5, nullptr) != pdPASS) {
        ESP_LOGE(TAG, "UI task creation failed");
        return false;
    }

    return true;
}

void ui_post_status(const char *text)
{
    if (s_status_queue == nullptr || text == nullptr) {
        return;
    }
    StatusMessage msg = {};
    std::snprintf(msg.text, sizeof(msg.text), "%s", text);
    xQueueSend(s_status_queue, &msg, 0);
}

}  // namespace nex
