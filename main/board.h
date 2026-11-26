/*
 * NEX firmware — board support
 * SPDX-License-Identifier: MIT
 *
 * Register-level facts in this file are derived from the M5Stack STOPWATCH
 * (C152) schematic, the M5PM1 / M5IOE1 datasheets, and the MIT-licensed
 * M5StopWatch-UserDemo 0.5 reference implementation.
 */
#pragma once

#include <cstdint>
#include <M5GFX.h>
#include "driver/i2c_master.h"

namespace nex {

// Initialise I2C, M5PM1, M5IOE1, peripheral power (L3B) and panel resets.
// Returns false on any fatal failure; the caller must stop safely.
bool board_init();

// Display object owned by the board layer (valid after board_init()).
lgfx::LGFX_Device &board_display();

// System I2C bus (valid after board_init()); used by the RTC service.
i2c_master_bus_handle_t board_i2c_bus();

// CST820 is optional at the board level: on failure the UI still comes up
// and reports the failure instead of hanging.
bool board_touch_init();

// Read one touch sample. Returns false only on I2C failure.
bool board_touch_read(uint16_t &x, uint16_t &y, bool &pressed);

}  // namespace nex
