/*
 * NEX firmware — LVGL UI
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <cstdint>

namespace nex {

// Build the Hello World UI and start the LVGL task.
// Returns false if the display or the draw buffers cannot be set up.
bool ui_start();

// Queue a physical key event for the UI task. The UI task forwards it to the
// Rust core; LVGL is never touched from the key task.
void ui_post_key_event(int32_t event);

}  // namespace nex
