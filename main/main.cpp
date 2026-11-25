/*
 * NEX firmware — entry point
 * SPDX-License-Identifier: MIT
 */
#include <esp_log.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "board.h"
#include "ui.h"

static const char *TAG = "NEX";

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

    ESP_LOGI(TAG, "UI running");

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
