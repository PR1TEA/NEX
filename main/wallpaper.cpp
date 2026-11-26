/*
 * NEX firmware — wallpaper storage and upload access point
 * SPDX-License-Identifier: MIT
 *
 * The phone joins a temporary WPA2 AP and POSTs a JPEG to /upload. The file is
 * stored in the SPIFFS "storage" partition and decoded by LVGL (tjpgd).
 */
#include "wallpaper.h"

#include <cstdio>
#include <cstring>

#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_spiffs.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace nex {
namespace {

constexpr const char *TAG = "NEX-WALL";
constexpr const char *kMountPoint = "/spiffs";
constexpr const char *kFilePath = "/spiffs/wall.jpg";
constexpr const char *kLvglPath = "S:/wall.jpg";
constexpr const char *kSsid = "NEX-Watch";
constexpr const char *kPassword = "nexwatch1234";
constexpr size_t kMaxUpload = 512 * 1024;
constexpr uint32_t kApTimeoutMs = 5 * 60 * 1000;

bool s_mounted = false;
bool s_ap_active = false;
int s_generation = 0;
httpd_handle_t s_server = nullptr;
bool s_wifi_inited = false;

esp_err_t upload_handler(httpd_req_t *req);
esp_err_t index_handler(httpd_req_t *req);

httpd_uri_t s_index_uri = {.uri = "/", .method = HTTP_GET, .handler = index_handler, .user_ctx = nullptr};
httpd_uri_t s_upload_uri = {.uri = "/upload", .method = HTTP_POST, .handler = upload_handler, .user_ctx = nullptr};
esp_netif_t *s_ap_netif = nullptr;
esp_timer_handle_t s_timeout = nullptr;

void ap_timeout_cb(void *)
{
    ESP_LOGI(TAG, "upload AP timed out");
    wallpaper_ap_stop();
}

esp_err_t upload_handler(httpd_req_t *req)
{
    const size_t total = req->content_len;
    if (total == 0 || total > kMaxUpload) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad length");
        return ESP_FAIL;
    }

    auto *buffer = static_cast<uint8_t *>(heap_caps_malloc(total, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (buffer == nullptr) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory");
        return ESP_FAIL;
    }

    size_t received = 0;
    while (received < total) {
        const int result = httpd_req_recv(req, reinterpret_cast<char *>(buffer) + received, total - received);
        if (result <= 0) {
            heap_caps_free(buffer);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "recv failed");
            return ESP_FAIL;
        }
        received += static_cast<size_t>(result);
    }

    const bool ok = wallpaper_save(buffer, total);
    heap_caps_free(buffer);

    if (!ok) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "save failed");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "wallpaper uploaded: %u bytes", (unsigned)total);
    httpd_resp_sendstr(req, "OK");
    return ESP_OK;
}

esp_err_t index_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, "NEX wallpaper upload: POST JPEG to /upload\n");
    return ESP_OK;
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

bool wallpaper_ap_start()
{
    if (s_ap_active) {
        return true;
    }

    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_netif_init());
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_event_loop_create_default());
    if (s_ap_netif == nullptr) {
        s_ap_netif = esp_netif_create_default_wifi_ap();
    }

    if (!s_wifi_inited) {
        wifi_init_config_t wifi_config = WIFI_INIT_CONFIG_DEFAULT();
        const esp_err_t init_err = esp_wifi_init(&wifi_config);
        if (init_err != ESP_OK && init_err != ESP_ERR_WIFI_INIT_STATE) {
            ESP_LOGE(TAG, "wifi init failed: %s", esp_err_to_name(init_err));
            return false;
        }
        s_wifi_inited = true;
    }

    wifi_config_t ap_config = {};
    std::snprintf(reinterpret_cast<char *>(ap_config.ap.ssid), sizeof(ap_config.ap.ssid), "%s", kSsid);
    std::snprintf(reinterpret_cast<char *>(ap_config.ap.password), sizeof(ap_config.ap.password), "%s", kPassword);
    ap_config.ap.ssid_len = std::strlen(kSsid);
    ap_config.ap.channel = 1;
    ap_config.ap.max_connection = 2;
    ap_config.ap.authmode = WIFI_AUTH_WPA2_PSK;

    esp_wifi_set_mode(WIFI_MODE_AP);
    esp_wifi_set_config(WIFI_IF_AP, &ap_config);
    const esp_err_t start_err = esp_wifi_start();
    if (start_err != ESP_OK) {
        ESP_LOGE(TAG, "wifi AP start failed: %s", esp_err_to_name(start_err));
        return false;
    }
    ESP_LOGI(TAG, "wifi AP started");

    if (s_server == nullptr) {
        httpd_config_t http_config = HTTPD_DEFAULT_CONFIG();
        http_config.max_uri_handlers = 4;
        http_config.lru_purge_enable = true;
        http_config.stack_size = 8192;
        const esp_err_t http_err = httpd_start(&s_server, &http_config);
        if (http_err != ESP_OK) {
            ESP_LOGE(TAG, "http server start failed: %s", esp_err_to_name(http_err));
            s_server = nullptr;
            esp_wifi_stop();
            return false;
        }
        httpd_register_uri_handler(s_server, &s_index_uri);
        httpd_register_uri_handler(s_server, &s_upload_uri);
        ESP_LOGI(TAG, "http server listening on :80");
    }

    const esp_timer_create_args_t timer_args = {
        .callback = ap_timeout_cb,
        .arg = nullptr,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "wall_ap_timeout",
        .skip_unhandled_events = true,
    };
    if (s_timeout == nullptr) {
        esp_timer_create(&timer_args, &s_timeout);
    }
    esp_timer_stop(s_timeout);
    esp_timer_start_once(s_timeout, kApTimeoutMs * 1000ULL);

    s_ap_active = true;
    ESP_LOGI(TAG, "upload AP '%s' started", kSsid);
    return true;
}

void wallpaper_ap_stop()
{
    if (!s_ap_active) {
        return;
    }
    if (s_timeout != nullptr) {
        esp_timer_stop(s_timeout);
    }
    if (s_server != nullptr) {
        httpd_stop(s_server);
        s_server = nullptr;
    }
    esp_wifi_stop();
    s_ap_active = false;
    ESP_LOGI(TAG, "upload AP stopped");
}

bool wallpaper_ap_active()
{
    return s_ap_active;
}

const char *wallpaper_ap_ssid()
{
    return kSsid;
}

const char *wallpaper_ap_password()
{
    return kPassword;
}

}  // namespace nex
