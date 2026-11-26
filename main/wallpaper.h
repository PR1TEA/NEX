/*
 * NEX firmware — wallpaper storage
 * SPDX-License-Identifier: GPL-3.0-or-later
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
void wallpaper_lvgl_fs_init();

}  // namespace nex
