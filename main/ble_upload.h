/*
 * NEX firmware — BLE wallpaper upload service
 * SPDX-License-Identifier: MIT
 */
#pragma once

namespace nex {

bool ble_upload_init();
void ble_upload_advertise(bool enabled);
bool ble_upload_connected();
int ble_upload_progress();
int ble_upload_status();

}  // namespace nex
