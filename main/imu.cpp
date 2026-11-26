/*
 * NEX firmware — BMI270 IMU service
 * SPDX-License-Identifier: MIT
 *
 * The IMU sits on the shared system I2C bus at 0x68 (3V3_L2).
 */
#include "imu.h"

#include "bmi270.h"
#include "board.h"
#include "esp_log.h"

namespace nex {
namespace {

constexpr const char *TAG = "NEX-IMU";
bmi270_handle_t *s_imu = nullptr;

}  // namespace

bool imu_service_init()
{
    if (s_imu != nullptr) {
        return true;
    }

    bmi270_driver_config_t driver = {};
    driver.addr = BMI270_I2C_ADDRESS_L;
    driver.interface = BMI270_USE_I2C;
    driver.i2c_bus = board_i2c_bus();

    if (bmi270_create(&driver, &s_imu) != ESP_OK || s_imu == nullptr) {
        ESP_LOGW(TAG, "BMI270 not found at 0x%02X", driver.addr);
        s_imu = nullptr;
        return false;
    }

    uint8_t chip_id = 0;
    if (bmi270_get_chip_id(s_imu, &chip_id) != ESP_OK || chip_id != BMI270_CHIP_ID) {
        ESP_LOGE(TAG, "BMI270 chip id mismatch: 0x%02X", chip_id);
        bmi270_delete(s_imu);
        s_imu = nullptr;
        return false;
    }

    bmi270_config_t config = {};
    config.acce_odr = BMI270_ACC_ODR_100_HZ;
    config.acce_range = BMI270_ACC_RANGE_4_G;
    config.gyro_odr = BMI270_GYR_ODR_100_HZ;
    config.gyro_range = BMI270_GYR_RANGE_500_DPS;
    if (bmi270_start(s_imu, &config) != ESP_OK) {
        ESP_LOGE(TAG, "BMI270 start failed");
        bmi270_delete(s_imu);
        s_imu = nullptr;
        return false;
    }

    ESP_LOGI(TAG, "BMI270 ready at 0x%02X", driver.addr);
    return true;
}

bool imu_read(float &ax, float &ay, float &az, float &gx, float &gy, float &gz)
{
    if (s_imu == nullptr) {
        return false;
    }
    return bmi270_get_acce_data(s_imu, &ax, &ay, &az) == ESP_OK &&
           bmi270_get_gyro_data(s_imu, &gx, &gy, &gz) == ESP_OK;
}

}  // namespace nex
