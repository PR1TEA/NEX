/*
 * NEX firmware — BMI270 IMU service
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

namespace nex {

bool imu_service_init();
bool imu_read(float &ax, float &ay, float &az, float &gx, float &gy, float &gz);

}  // namespace nex
