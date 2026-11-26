/*
 * NEX firmware — wallpaper storage
 * SPDX-License-Identifier: MIT
 *
 * JPEG files uploaded over BLE are stored in the SPIFFS "storage" partition
 * and decoded by LVGL (tjpgd) for the watchface background.
 */
#include "wallpaper.h"

#include <cstdio>

#include "esp_log.h"
#include "esp_spiffs.h"

namespace nex {
namespace {

constexpr const char *TAG = "NEX-WALL";
constexpr const char *kMountPoint = "/spiffs";
constexpr const char *kFilePath = "/spiffs/wall.jpg";
constexpr const char *kLvglPath = "S:/wall.jpg";

bool s_mounted = false;
int s_generation = 0;

}  // namespace

bool wallpaper_init()
{
    if (s_mounted) {
        return true;
    }
    esp_vfs_spiffs_conf_t conf = {};
    conf.base_path = kMountPoint;
    conf.partition_label = "storage";
    conf.max_files = 4;
    conf.format_if_mount_failed = true;

    const esp_err_t err = esp_vfs_spiffs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SPIFFS mount failed: %s", esp_err_to_name(err));
        return false;
    }
    s_mounted = true;
    ESP_LOGI(TAG, "storage mounted, wallpaper %s", wallpaper_exists() ? "present" : "absent");
    return true;
}

bool wallpaper_exists()
{
    FILE *file = std::fopen(kFilePath, "rb");
    if (file == nullptr) {
        return false;
    }
    std::fclose(file);
    return true;
}

bool wallpaper_save(const uint8_t *data, size_t length)
{
    if (!s_mounted && !wallpaper_init()) {
        return false;
    }
    FILE *file = std::fopen(kFilePath, "wb");
    if (file == nullptr) {
        ESP_LOGE(TAG, "cannot open wallpaper for writing");
        return false;
    }
    const size_t written = std::fwrite(data, 1, length, file);
    std::fclose(file);
    if (written != length) {
        ESP_LOGE(TAG, "short write: %u/%u", (unsigned)written, (unsigned)length);
        return false;
    }
    ++s_generation;
    return true;
}

int wallpaper_generation()
{
    return s_generation;
}

const char *wallpaper_lvgl_path()
{
    return kLvglPath;
}

}  // namespace nex
