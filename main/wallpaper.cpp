/*
 * NEX firmware — wallpaper storage
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * JPEG files uploaded over BLE are stored in the SPIFFS "storage" partition
 * and decoded by LVGL (tjpgd) for the watchface background.
 */
#include "wallpaper.h"

#include <cstdio>

#include "esp_log.h"
#include "esp_spiffs.h"
#include "lvgl.h"

namespace nex {
namespace {

constexpr const char *TAG = "NEX-WALL";
constexpr const char *kMountPoint = "/spiffs";
constexpr const char *kFilePath = "/spiffs/wall.jpg";
constexpr const char *kLvglPath = "W:/wall.jpg";

bool s_mounted = false;
int s_generation = 0;
bool s_fs_registered = false;

void *fs_open(lv_fs_drv_t *, const char *path, lv_fs_mode_t mode)
{
    char full[128];
    std::snprintf(full, sizeof(full), "/spiffs%s", path);
    return std::fopen(full, (mode & LV_FS_MODE_WR) != 0 ? "wb" : "rb");
}

lv_fs_res_t fs_close(lv_fs_drv_t *, void *file)
{
    if (file != nullptr) {
        std::fclose(static_cast<FILE *>(file));
    }
    return LV_FS_RES_OK;
}

lv_fs_res_t fs_read(lv_fs_drv_t *, void *file, void *buffer, uint32_t bytes_to_read, uint32_t *bytes_read)
{
    auto *stream = static_cast<FILE *>(file);
    size_t total = 0;
    while (total < bytes_to_read) {
        const size_t got = std::fread(static_cast<uint8_t *>(buffer) + total, 1, bytes_to_read - total, stream);
        if (got == 0) {
            break;
        }
        total += got;
    }
    *bytes_read = static_cast<uint32_t>(total);
    return LV_FS_RES_OK;
}

lv_fs_res_t fs_seek(lv_fs_drv_t *, void *file, uint32_t position, lv_fs_whence_t whence)
{
    int origin = SEEK_SET;
    if (whence == LV_FS_SEEK_CUR) origin = SEEK_CUR;
    if (whence == LV_FS_SEEK_END) origin = SEEK_END;
    return std::fseek(static_cast<FILE *>(file), static_cast<long>(position), origin) == 0 ? LV_FS_RES_OK
                                                                                          : LV_FS_RES_FS_ERR;
}

lv_fs_res_t fs_tell(lv_fs_drv_t *, void *file, uint32_t *position)
{
    const long pos = std::ftell(static_cast<FILE *>(file));
    if (pos < 0) {
        return LV_FS_RES_FS_ERR;
    }
    *position = static_cast<uint32_t>(pos);
    return LV_FS_RES_OK;
}

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
    FILE *probe = std::fopen(kFilePath, "rb");
    if (probe != nullptr) {
        std::fseek(probe, 0, SEEK_END);
        const long size = std::ftell(probe);
        std::fseek(probe, 0, SEEK_SET);
        uint8_t head[4] = {};
        const size_t got = std::fread(head, 1, sizeof(head), probe);
        std::fclose(probe);
        ESP_LOGI(TAG, "wallpaper size=%ld head=%02X %02X %02X %02X (%u)", size,
                 head[0], head[1], head[2], head[3], (unsigned)got);
    }
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
    if (length < 4 || data[0] != 0xFF || data[1] != 0xD8) {
        ESP_LOGE(TAG, "not a JPEG");
        return false;
    }
    std::remove(kFilePath);
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

void wallpaper_lvgl_fs_init()
{
    if (s_fs_registered) {
        return;
    }
    lv_fs_drv_t driver = {};
    lv_fs_drv_init(&driver);
    driver.letter = 'W';
    driver.open_cb = fs_open;
    driver.close_cb = fs_close;
    driver.read_cb = fs_read;
    driver.seek_cb = fs_seek;
    driver.tell_cb = fs_tell;
    lv_fs_drv_register(&driver);
    s_fs_registered = true;
}

const char *wallpaper_lvgl_path()
{
    return kLvglPath;
}

}  // namespace nex
