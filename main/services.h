/*
 * NEX firmware — system services interface
 * SPDX-License-Identifier: MIT
 */
#pragma once

namespace nex {

// NVS + RX8130CE bring-up. Returns true when NVS is usable.
bool services_init();

}  // namespace nex
