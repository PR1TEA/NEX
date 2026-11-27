/*
 * NEX firmware — serialized work queue for UI-owned resources
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * BLE callbacks must not touch SPIFFS, LVGL or the clock state directly: doing
 * so races with the UI task and can block the NimBLE host. They enqueue a job
 * instead and the UI task executes it in order.
 */
#include "ui_work.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

namespace nex {
namespace {

constexpr const char *TAG = "NEX-WORK";
constexpr int kQueueLength = 8;

QueueHandle_t s_queue = nullptr;

}  // namespace

bool ui_work_init()
{
    if (s_queue != nullptr) {
        return true;
    }
    s_queue = xQueueCreate(kQueueLength, sizeof(WorkFn) + sizeof(void *));
    if (s_queue == nullptr) {
        ESP_LOGE(TAG, "queue create failed");
        return false;
    }
    return true;
}

bool ui_work_post(WorkFn fn, void *arg)
{
    if (s_queue == nullptr || fn == nullptr) {
        return false;
    }
    struct Job {
        WorkFn fn;
        void *arg;
    } job = {fn, arg};
    if (xQueueSend(s_queue, &job, 0) != pdTRUE) {
        ESP_LOGW(TAG, "queue full, dropping job");
        return false;
    }
    return true;
}

int ui_work_process()
{
    if (s_queue == nullptr) {
        return 0;
    }
    int processed = 0;
    struct Job {
        WorkFn fn;
        void *arg;
    } job = {};
    while (xQueueReceive(s_queue, &job, 0) == pdTRUE) {
        job.fn(job.arg);
        ++processed;
    }
    return processed;
}

}  // namespace nex
