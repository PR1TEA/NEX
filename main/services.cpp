/*
 * NEX firmware — system services (C++ side of the Rust core ABI)
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * - RX8130CE access over the existing system I2C bus (0x32)
 * - NVS blob storage for the Rust settings structure
 * - monotonic milliseconds
 *
 * The RTC is always written in UTC. A 32-bit marker in the RTC RAM ("NEXU")
 * records that the stored time was written by NEX as UTC; without that marker
 * the content is treated as untrusted and the clock starts invalid.
 */
#include "nex_core.h"

#include <cstring>

#include "board.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"
#include "nvs_flash.h"

namespace nex {
namespace {

constexpr const char *TAG = "NEX-SVC";
constexpr uint8_t kRtcAddr = 0x32;

constexpr uint8_t REG_SEC = 0x10;
constexpr uint8_t REG_MIN = 0x11;
constexpr uint8_t REG_HOUR = 0x12;
constexpr uint8_t REG_DAY = 0x14;
constexpr uint8_t REG_MONTH = 0x15;
constexpr uint8_t REG_YEAR = 0x16;
constexpr uint8_t REG_FLAG = 0x1D;
constexpr uint8_t REG_CTRL0 = 0x1E;
constexpr uint8_t REG_RAM = 0x20;

constexpr uint8_t kMagic[4] = {'N', 'E', 'X', 'U'};

i2c_master_dev_handle_t s_rtc = nullptr;
bool s_nvs_ready = false;

bool bcd_to_int(uint8_t value, int &out)
{
    const uint8_t hi = value >> 4;
    const uint8_t lo = value & 0x0F;
    if (hi > 9 || lo > 9) {
        return false;
    }
    out = hi * 10 + lo;
    return true;
}

uint8_t int_to_bcd(int value)
{
    return static_cast<uint8_t>(((value / 10) << 4) | (value % 10));
}

bool read_regs(uint8_t reg, uint8_t *out, size_t len)
{
    if (s_rtc == nullptr) {
        return false;
    }
    return i2c_master_transmit_receive(s_rtc, &reg, 1, out, len, 100) == ESP_OK;
}

bool write_reg(uint8_t reg, uint8_t value)
{
    if (s_rtc == nullptr) {
        return false;
    }
    uint8_t buf[2] = {reg, value};
    return i2c_master_transmit(s_rtc, buf, sizeof(buf), 100) == ESP_OK;
}

int32_t nvs_load(uint8_t *buf, int32_t len)
{
    if (!s_nvs_ready || buf == nullptr || len <= 0) {
        return -1;
    }
    nvs_handle_t handle;
    if (nvs_open("nex", NVS_READONLY, &handle) != ESP_OK) {
        return -1;
    }
    size_t size = static_cast<size_t>(len);
    const esp_err_t err = nvs_get_blob(handle, "core", buf, &size);
    nvs_close(handle);
    if (err != ESP_OK) {
        return -1;
    }
    return static_cast<int32_t>(size);
}

int32_t nvs_save(const uint8_t *buf, int32_t len)
{
    if (!s_nvs_ready || buf == nullptr || len <= 0) {
        return -1;
    }
    nvs_handle_t handle;
    if (nvs_open("nex", NVS_READWRITE, &handle) != ESP_OK) {
        return -1;
    }
    const esp_err_t err = nvs_set_blob(handle, "core", buf, static_cast<size_t>(len));
    if (err == ESP_OK) {
        nvs_commit(handle);
    }
    nvs_close(handle);
    return err == ESP_OK ? 0 : -1;
}

}  // namespace

bool services_init()
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS needs erase (%s), reformatting", esp_err_to_name(err));
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    s_nvs_ready = (err == ESP_OK);
    if (!s_nvs_ready) {
        ESP_LOGE(TAG, "NVS init failed: %s", esp_err_to_name(err));
    }

    i2c_master_bus_handle_t bus = board_i2c_bus();
    if (bus != nullptr) {
        i2c_device_config_t cfg = {};
        cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
        cfg.device_address = kRtcAddr;
        cfg.scl_speed_hz = 100000;
        if (i2c_master_bus_add_device(bus, &cfg, &s_rtc) != ESP_OK) {
            s_rtc = nullptr;
        }
    }
    if (s_rtc == nullptr) {
        ESP_LOGW(TAG, "RX8130CE not available at 0x%02X", kRtcAddr);
    }
    return s_nvs_ready;
}

}  // namespace nex

extern "C" int32_t nex_rtc_read(nex_rtc_fields_t *out)
{
    if (out == nullptr) {
        return -1;
    }
    std::memset(out, 0, sizeof(*out));

    uint8_t raw[7] = {};  // 0x10..0x16
    uint8_t flag = 0;
    uint8_t ctrl0 = 0;
    uint8_t magic[4] = {};

    const bool ok = nex::read_regs(nex::REG_SEC, raw, sizeof(raw)) &&
                    nex::read_regs(nex::REG_FLAG, &flag, 1) &&
                    nex::read_regs(nex::REG_CTRL0, &ctrl0, 1) &&
                    nex::read_regs(nex::REG_RAM, magic, sizeof(magic));

    if (!ok) {
        out->flags = 0;
        return -1;
    }

    int sec = 0;
    int min = 0;
    int hour = 0;
    int day = 0;
    int mon = 0;
    int year = 0;
    const bool bcd_ok = nex::bcd_to_int(raw[0], sec) && nex::bcd_to_int(raw[1], min) &&
                        nex::bcd_to_int(raw[2], hour) && nex::bcd_to_int(raw[4], day) &&
                        nex::bcd_to_int(raw[5], mon) && nex::bcd_to_int(raw[6], year);

    out->year = 2000 + year;
    out->month = mon;
    out->day = day;
    out->hour = hour;
    out->minute = min;
    out->second = sec;

    uint32_t flags = NEX_RTC_FLAG_I2C_OK;
    if (!bcd_ok || (flag & 0x02) != 0) {  // VLF
        flags |= NEX_RTC_FLAG_VLF;
    }
    if ((ctrl0 & 0x40) != 0) {  // STOP
        flags |= NEX_RTC_FLAG_STOPPED;
    }
    if (std::memcmp(magic, nex::kMagic, sizeof(magic)) == 0) {
        flags |= NEX_RTC_FLAG_MAGIC_OK;
    }
    out->flags = flags;
    return 0;
}

extern "C" int32_t nex_rtc_write(int32_t year, int32_t month, int32_t day, int32_t hour,
                                 int32_t minute, int32_t second)
{
    if (year < 2000 || year > 2099 || month < 1 || month > 12 || day < 1 || day > 31 ||
        hour < 0 || hour > 23 || minute < 0 || minute > 59 || second < 0 || second > 59) {
        return -2;
    }

    uint8_t ctrl0 = 0;
    if (!nex::read_regs(nex::REG_CTRL0, &ctrl0, 1)) {
        return -1;
    }
    if (!nex::write_reg(nex::REG_CTRL0, static_cast<uint8_t>(ctrl0 | 0x40))) {  // STOP
        return -1;
    }

    bool ok = true;
    ok = ok && nex::write_reg(nex::REG_SEC, nex::int_to_bcd(second));
    ok = ok && nex::write_reg(nex::REG_MIN, nex::int_to_bcd(minute));
    ok = ok && nex::write_reg(nex::REG_HOUR, nex::int_to_bcd(hour));
    ok = ok && nex::write_reg(nex::REG_DAY, nex::int_to_bcd(day));
    ok = ok && nex::write_reg(nex::REG_MONTH, nex::int_to_bcd(month));
    ok = ok && nex::write_reg(nex::REG_YEAR, nex::int_to_bcd(year % 100));

    // Clear STOP and the VLF flag (write 0 to VLF clears it).
    ok = ok && nex::write_reg(nex::REG_CTRL0, static_cast<uint8_t>(ctrl0 & ~0x40));
    ok = ok && nex::write_reg(nex::REG_FLAG, 0x00);

    const uint8_t magic[4] = {'N', 'E', 'X', 'U'};
    for (int i = 0; i < 4; ++i) {
        ok = ok && nex::write_reg(static_cast<uint8_t>(nex::REG_RAM + i), magic[i]);
    }
    return ok ? 0 : -1;
}

extern "C" int32_t nex_nvs_load(uint8_t *buf, int32_t len)
{
    return nex::nvs_load(buf, len);
}

extern "C" int32_t nex_nvs_save(const uint8_t *buf, int32_t len)
{
    return nex::nvs_save(buf, len);
}

extern "C" uint64_t nex_monotonic_ms(void)
{
    return static_cast<uint64_t>(esp_timer_get_time() / 1000);
}
