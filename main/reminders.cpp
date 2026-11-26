/*
 * NEX firmware — reminders (checklist) storage
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Reminders are stored as JSON in /spiffs/reminders.json and synced with the
 * phone over BLE. The device is the source of truth for `done`.
 */
#include "reminders.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "cJSON.h"
#include "esp_log.h"

namespace nex {
namespace {

constexpr const char *TAG = "NEX-TODO";
constexpr const char *kPath = "/spiffs/reminders.json";
constexpr const char *kTmpPath = "/spiffs/reminders.tmp";

Reminder s_items[kReminderMax];
int s_count = 0;
int s_generation = 0;

void copy_text(char *destination, const char *source)
{
    size_t length = std::strlen(source);
    if (length > kReminderTextMax) {
        length = kReminderTextMax;
        while (length > 0 && (source[length] & 0xC0) == 0x80) {
            --length;
        }
    }
    std::memcpy(destination, source, length);
    destination[length] = '\0';
}

bool save()
{
    cJSON *root = cJSON_CreateObject();
    if (root == nullptr) {
        return false;
    }
    cJSON_AddNumberToObject(root, "v", 1);
    cJSON *items = cJSON_AddArrayToObject(root, "items");
    for (int i = 0; i < s_count; ++i) {
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "text", s_items[i].text);
        cJSON_AddBoolToObject(item, "done", s_items[i].done);
        cJSON_AddItemToArray(items, item);
    }
    char *serialized = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (serialized == nullptr) {
        return false;
    }

    bool ok = false;
    FILE *file = std::fopen(kTmpPath, "wb");
    if (file != nullptr) {
        const size_t length = std::strlen(serialized);
        const size_t written = std::fwrite(serialized, 1, length, file);
        std::fclose(file);
        if (written == length) {
            std::remove(kPath);
            ok = std::rename(kTmpPath, kPath) == 0;
        }
    }
    cJSON_free(serialized);
    if (!ok) {
        ESP_LOGE(TAG, "save failed");
    }
    return ok;
}

bool apply_json(const char *json, size_t length, bool persist)
{
    cJSON *root = cJSON_ParseWithLength(json, length);
    if (root == nullptr) {
        ESP_LOGW(TAG, "invalid JSON");
        return false;
    }
    cJSON *items = cJSON_GetObjectItem(root, "items");
    if (!cJSON_IsArray(items)) {
        cJSON_Delete(root);
        return false;
    }

    const int incoming = cJSON_GetArraySize(items);
    s_count = 0;
    for (int i = 0; i < incoming && s_count < kReminderMax; ++i) {
        cJSON *item = cJSON_GetArrayItem(items, i);
        cJSON *text = cJSON_GetObjectItem(item, "text");
        if (!cJSON_IsString(text) || text->valuestring == nullptr) {
            continue;
        }
        copy_text(s_items[s_count].text, text->valuestring);
        const cJSON *done = cJSON_GetObjectItem(item, "done");
        s_items[s_count].done = cJSON_IsTrue(done);
        ++s_count;
    }
    cJSON_Delete(root);

    if (persist) {
        save();
    }
    ++s_generation;
    ESP_LOGI(TAG, "loaded %d reminder(s)", s_count);
    return true;
}

}  // namespace

bool reminders_init()
{
    FILE *file = std::fopen(kPath, "rb");
    if (file == nullptr) {
        s_count = 0;
        return true;
    }
    std::fseek(file, 0, SEEK_END);
    const long length = std::ftell(file);
    std::fseek(file, 0, SEEK_SET);
    if (length <= 0 || length > 8192) {
        std::fclose(file);
        s_count = 0;
        return true;
    }
    char *buffer = static_cast<char *>(std::malloc(static_cast<size_t>(length)));
    if (buffer == nullptr) {
        std::fclose(file);
        return false;
    }
    const size_t read = std::fread(buffer, 1, static_cast<size_t>(length), file);
    std::fclose(file);
    const bool ok = read == static_cast<size_t>(length) && apply_json(buffer, static_cast<size_t>(length), false);
    std::free(buffer);
    return ok;
}

int reminders_count()
{
    return s_count;
}

const Reminder *reminders_get(int index)
{
    if (index < 0 || index >= s_count) {
        return nullptr;
    }
    return &s_items[index];
}

bool reminders_toggle(int index)
{
    if (index < 0 || index >= s_count) {
        return false;
    }
    s_items[index].done = !s_items[index].done;
    ++s_generation;
    return save();
}

bool reminders_delete(int index)
{
    if (index < 0 || index >= s_count) {
        return false;
    }
    for (int i = index; i + 1 < s_count; ++i) {
        s_items[i] = s_items[i + 1];
    }
    --s_count;
    ++s_generation;
    return save();
}

bool reminders_append_json(const char *json, size_t length)
{
    cJSON *root = cJSON_ParseWithLength(json, length);
    if (root == nullptr) {
        ESP_LOGW(TAG, "append: invalid JSON (%u bytes)", (unsigned)length);
        return false;
    }
    cJSON *items = cJSON_GetObjectItem(root, "items");
    if (!cJSON_IsArray(items)) {
        ESP_LOGW(TAG, "append: no items array");
        cJSON_Delete(root);
        return false;
    }
    const int incoming = cJSON_GetArraySize(items);
    ESP_LOGI(TAG, "append: %d item(s), have %d", incoming, s_count);
    for (int i = 0; i < incoming && s_count < kReminderMax; ++i) {
        cJSON *item = cJSON_GetArrayItem(items, i);
        cJSON *text = cJSON_GetObjectItem(item, "text");
        if (!cJSON_IsString(text) || text->valuestring == nullptr) {
            continue;
        }
        copy_text(s_items[s_count].text, text->valuestring);
        s_items[s_count].done = false;
        ++s_count;
    }
    cJSON_Delete(root);
    ++s_generation;
    const bool ok = save();
    ESP_LOGI(TAG, "append: save=%d count=%d", ok ? 1 : 0, s_count);
    return ok;
}

bool reminders_set_json(const char *json, size_t length)
{
    return apply_json(json, length, true);
}

int reminders_generation()
{
    return s_generation;
}

int reminders_to_json(char *out, size_t capacity)
{
    cJSON *root = cJSON_CreateObject();
    if (root == nullptr) {
        return -1;
    }
    cJSON_AddNumberToObject(root, "v", 1);
    cJSON *items = cJSON_AddArrayToObject(root, "items");
    for (int i = 0; i < s_count; ++i) {
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "text", s_items[i].text);
        cJSON_AddBoolToObject(item, "done", s_items[i].done);
        cJSON_AddItemToArray(items, item);
    }
    char *serialized = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (serialized == nullptr) {
        return -1;
    }
    const size_t length = std::strlen(serialized);
    if (length + 1 > capacity) {
        cJSON_free(serialized);
        return -1;
    }
    std::memcpy(out, serialized, length + 1);
    cJSON_free(serialized);
    return static_cast<int>(length);
}

}  // namespace nex
