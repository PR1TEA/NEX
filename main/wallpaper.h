/*
 * NEX firmware — wallpaper storage and upload access point
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <cstddef>
#include <cstdint>

namespace nex {

bool wallpaper_init();
bool wallpaper_exists();
bool wallpaper_save(const uint8_t *data, size_t length);
int wallpaper_generation();
const char *wallpaper_lvgl_path();

bool wallpaper_ap_start();
void wallpaper_ap_stop();
bool wallpaper_ap_active();
const char *wallpaper_ap_ssid();
const char *wallpaper_ap_password();

}  // namespace nex
