/*
 * NEX firmware — entry point
 * SPDX-License-Identifier: MIT
 */
#include <esp_log.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "board.h"
#include "keys.h"
#include "nex_rust.h"
#include "ui.h"

static const char *TAG = "NEX";

// Called from Rust through the C ABI declared in nex_rust.h.
extern "C" void nex_log_from_rust(const char *message)
{
    ESP_LOGI("NEX_RUST", "%s", message != nullptr ? message : "(null)");
}

extern "C" void app_main(void)
{
    ESP_LOGI(TAG, "NEX Hello World starting (ESP-IDF %s)", esp_get_idf_version());

    if (!nex::board_init()) {
        ESP_LOGE(TAG, "board init failed - system halted");
        vTaskDelay(portMAX_DELAY);
        return;
    }

    if (!nex::ui_start()) {
        ESP_LOGE(TAG, "UI init failed - system halted");
        vTaskDelay(portMAX_DELAY);
        return;
    }

    if (!nex::keys_start()) {
        ESP_LOGE(TAG, "physical key init failed");
    }

    const int32_t rust_sum = nex_rust_add(20, 22);
    ESP_LOGI(TAG, "Rust interop: nex_rust_add(20, 22) = %d (expected 42)", (int)rust_sum);
    nex_rust_hello();

    ESP_LOGI(TAG, "UI running");

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
