/*
 * NEX firmware — LVGL UI
 * SPDX-License-Identifier: MIT
 */
#pragma once

namespace nex {

// Build the Hello World UI and start the LVGL task.
// Returns false if the display or the draw buffers cannot be set up.
bool ui_start();

// Post a status string to the LVGL thread. Safe to call from any task; the
// string is queued and applied inside the UI task under the LVGL mutex.
void ui_post_status(const char *text);

}  // namespace nex
