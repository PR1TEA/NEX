/*
 * NEX firmware — board support
 * SPDX-License-Identifier: MIT
 *
 * M5Stack STOPWATCH C152 board bring-up:
 *   - system I2C on GPIO47/48
 *   - M5PM1 power management (0x6E)
 *   - M5IOE1 IO expander (0x4F or 0x6F, probed, never written blindly)
 *   - 3V3_L3B peripheral rail, OLED/touch reset
 *   - CO5300 AMOLED over QSPI (SPI2_HOST)
 *   - CST820 touch controller (0x15)
 *
 * Panel init table and QSPI parameters follow M5GFX 0.2.19 Panel_AMOLED and
 * the M5StopWatch-UserDemo 0.5 (MIT), Copyright (c) M5Stack Technology CO LTD.
 */
#include "board.h"

#include <cstdio>
#include <cstring>

#include "driver/i2c_master.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lgfx/v1/panel/Panel_AMOLED.hpp"

namespace nex {
namespace {

constexpr const char *TAG = "NEX-BOARD";

// ---------------------------------------------------------------- I2C pins
constexpr gpio_num_t kI2cSda = GPIO_NUM_47;
constexpr gpio_num_t kI2cScl = GPIO_NUM_48;
constexpr uint32_t kI2cHz = 100000;

// ------------------------------------------------------------- I2C device
constexpr uint8_t kPmicAddr = 0x6E;   // M5PM1 (datasheet p.3)
constexpr uint8_t kIoeAddrA = 0x4F;   // STOPWATCH schematic p.4
constexpr uint8_t kIoeAddrB = 0x6F;   // M5IOE1 datasheet / driver default
constexpr uint8_t kTouchAddr = 0x15;  // CST820 (datasheet p.7)

// --------------------------------------------------------- M5IOE1 pin map
// Pin index = IO number - 1 (M5IOE1_PIN_1 == 0).
constexpr uint8_t kIoePinMuxCtrl = 0;      // IO1  UART/USB mux
constexpr uint8_t kIoePinAudioEn = 2;      // IO3  audio enable
constexpr uint8_t kIoePinTouchRst = 3;     // IO4  touch reset
constexpr uint8_t kIoePinOledRst = 4;      // IO5  OLED reset
constexpr uint8_t kIoePinL3bEn = 7;        // IO8  3V3_L3B enable
constexpr uint8_t kIoePinSpeakerPa = 9;    // IO10 speaker PA enable

// ------------------------------------------------------------- panel pins
constexpr gpio_num_t kPinSclk = GPIO_NUM_40;
constexpr gpio_num_t kPinIo0 = GPIO_NUM_41;
constexpr gpio_num_t kPinIo1 = GPIO_NUM_42;
constexpr gpio_num_t kPinIo2 = GPIO_NUM_46;
constexpr gpio_num_t kPinIo3 = GPIO_NUM_45;
constexpr gpio_num_t kPinCs = GPIO_NUM_39;
constexpr gpio_num_t kPinTe = GPIO_NUM_38;

constexpr uint32_t kPanelWindowW = 468;
constexpr uint32_t kPanelWindowH = 466;
constexpr uint16_t kPanelOffsetX = 6;

i2c_master_bus_handle_t s_bus = nullptr;
i2c_master_dev_handle_t s_pmic = nullptr;
i2c_master_dev_handle_t s_ioe = nullptr;
i2c_master_dev_handle_t s_touch = nullptr;
bool s_touch_ok = false;
uint8_t s_ioe_addr = 0;

bool i2c_write_reg(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t value)
{
    uint8_t buf[2] = {reg, value};
    return i2c_master_transmit(dev, buf, sizeof(buf), 100) == ESP_OK;
}

bool i2c_read_regs(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t *out, size_t len)
{
    return i2c_master_transmit_receive(dev, &reg, 1, out, len, 100) == ESP_OK;
}

i2c_master_dev_handle_t i2c_add_device(uint8_t addr)
{
    i2c_device_config_t cfg = {};
    cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    cfg.device_address = addr;
    cfg.scl_speed_hz = kI2cHz;

    i2c_master_dev_handle_t dev = nullptr;
    if (i2c_master_bus_add_device(s_bus, &cfg, &dev) != ESP_OK) {
        return nullptr;
    }
    return dev;
}

bool i2c_init()
{
    i2c_master_bus_config_t cfg = {};
    cfg.i2c_port = I2C_NUM_0;
    cfg.sda_io_num = kI2cSda;
    cfg.scl_io_num = kI2cScl;
    cfg.clk_source = I2C_CLK_SRC_DEFAULT;
    cfg.glitch_ignore_cnt = 7;
    cfg.flags.enable_internal_pullup = true;

    esp_err_t err = i2c_new_master_bus(&cfg, &s_bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2C bus init failed: %s", esp_err_to_name(err));
        return false;
    }
    ESP_LOGI(TAG, "I2C0 ready: SDA=%d SCL=%d @%uHz", (int)kI2cSda, (int)kI2cScl, (unsigned)kI2cHz);
    return true;
}

// ------------------------------------------------------------------ M5PM1
// Only two operations are performed here:
//   * read the device ID registers and log them (identity check)
//   * disable the PMIC watchdog (0x0A = 0)
// Charge current, low-voltage protection and eFuse settings are untouched.
bool pmic_init()
{
    s_pmic = i2c_add_device(kPmicAddr);
    if (s_pmic == nullptr) {
        ESP_LOGW(TAG, "PMIC not present at 0x%02X", kPmicAddr);
        return false;
    }

    uint8_t id[4] = {};
    if (!i2c_read_regs(s_pmic, 0x00, id, sizeof(id))) {
        ESP_LOGW(TAG, "PMIC at 0x%02X did not answer the ID read", kPmicAddr);
        return false;
    }
    ESP_LOGI(TAG, "M5PM1 found at 0x%02X: id=%02X %02X %02X %02X", kPmicAddr, id[0], id[1], id[2], id[3]);

    if (!i2c_write_reg(s_pmic, 0x0A, 0x00)) {
        ESP_LOGW(TAG, "PMIC watchdog could not be disabled");
    } else {
        uint8_t wdt = 0xFF;
        if (i2c_read_regs(s_pmic, 0x0A, &wdt, 1) && wdt == 0x00) {
            ESP_LOGI(TAG, "M5PM1 watchdog disabled (WDT_CNT reads 0)");
        } else {
            ESP_LOGW(TAG, "M5PM1 watchdog readback is 0x%02X, expected 0x00", wdt);
        }
    }
    return true;
}

// ----------------------------------------------------------------- M5IOE1
bool ioe_read(uint8_t reg, uint8_t &value)
{
    return i2c_read_regs(s_ioe, reg, &value, 1);
}

bool ioe_write(uint8_t reg, uint8_t value)
{
    return i2c_write_reg(s_ioe, reg, value);
}

bool ioe_reg_bit(uint8_t reg, uint8_t pin, bool set)
{
    uint8_t value = 0;
    if (!ioe_read(reg, value)) {
        return false;
    }
    const uint8_t mask = static_cast<uint8_t>(1u << (pin % 8));
    value = set ? (value | mask) : (value & ~mask);
    return ioe_write(reg, value);
}

uint8_t ioe_reg_for(uint8_t pin, uint8_t low, uint8_t high)
{
    return pin < 8 ? low : high;
}

bool ioe_set_level(uint8_t pin, int level)
{
    return ioe_reg_bit(ioe_reg_for(pin, 0x05, 0x06), pin, level != 0);
}

bool ioe_set_output(uint8_t pin)
{
    return ioe_reg_bit(ioe_reg_for(pin, 0x03, 0x04), pin, true);
}

bool ioe_set_push_pull(uint8_t pin)
{
    // DRV bit 0 = push-pull, 1 = open-drain.
    return ioe_reg_bit(ioe_reg_for(pin, 0x13, 0x14), pin, false);
}

bool ioe_read_pin(uint8_t pin)
{
    uint8_t value = 0;
    if (!ioe_read(ioe_reg_for(pin, 0x07, 0x08), value)) {
        return false;
    }
    return (value >> (pin % 8)) & 0x01;
}

bool ioe_probe()
{
    // Address candidates: the schematic value, the driver default, and any
    // other address that actually ACKs on the bus (the M5IOE1 straps its
    // address from IO7 and the documented range is 0x6F..0x76).
    uint8_t candidates[32] = {};
    size_t candidate_count = 0;
    auto add_candidate = [&](uint8_t addr) {
        for (size_t i = 0; i < candidate_count; ++i) {
            if (candidates[i] == addr) {
                return;
            }
        }
        if (candidate_count < sizeof(candidates)) {
            candidates[candidate_count++] = addr;
        }
    };

    add_candidate(kIoeAddrA);
    add_candidate(kIoeAddrB);

    uint8_t scan[32] = {};
    size_t scan_count = 0;
    for (uint16_t addr = 0x08; addr < 0x78; ++addr) {
        if (i2c_master_probe(s_bus, addr, 50) != ESP_OK) {
            continue;
        }
        if (scan_count < sizeof(scan)) {
            scan[scan_count++] = static_cast<uint8_t>(addr);
        }
        // Known devices are never treated as the IO expander.
        if (addr == 0x15 || addr == 0x18 || addr == 0x32 || addr == 0x68 || addr == 0x6E) {
            continue;
        }
        add_candidate(static_cast<uint8_t>(addr));
    }

    char list[3 * sizeof(scan) + 1] = {};
    size_t pos = 0;
    for (size_t i = 0; i < scan_count; ++i) {
        pos += snprintf(list + pos, sizeof(list) - pos, "%02X ", scan[i]);
    }
    ESP_LOGI(TAG, "I2C scan: %u device(s) [%s]", (unsigned)scan_count, list);

    for (size_t i = 0; i < candidate_count; ++i) {
        const uint8_t addr = candidates[i];
        i2c_master_dev_handle_t dev = i2c_add_device(addr);
        if (dev == nullptr) {
            continue;
        }

        // The expander can be in I2C sleep and NACK the first transfer, so
        // retry a few times instead of rejecting the address immediately.
        for (int attempt = 0; attempt < 3; ++attempt) {
            uint8_t uid[2] = {};
            uint8_t rev = 0;
            const bool uid_ok = i2c_read_regs(dev, 0x00, uid, sizeof(uid));
            const bool rev_ok = i2c_read_regs(dev, 0x02, &rev, 1);
            if (uid_ok && rev_ok) {
                const bool uid_valid = !(uid[0] == 0x00 && uid[1] == 0x00) &&
                                       !(uid[0] == 0xFF && uid[1] == 0xFF);
                const bool rev_valid = rev != 0x00 && rev != 0xFF;
                ESP_LOGI(TAG, "IOE candidate 0x%02X: uid=%02X%02X rev=0x%02X", addr, uid[0], uid[1], rev);
                if (uid_valid || rev_valid) {
                    s_ioe = dev;
                    s_ioe_addr = addr;
                    ESP_LOGI(TAG, "M5IOE1 found at 0x%02X (uid=%02X%02X rev=0x%02X)", addr, uid[0], uid[1], rev);
                    return true;
                }
            }
            vTaskDelay(pdMS_TO_TICKS(20));
        }

        ESP_LOGW(TAG, "IOE candidate 0x%02X rejected", addr);
        i2c_master_bus_rm_device(dev);
    }

    return false;
}

bool ioe_setup_outputs()
{
    struct PinInit {
        uint8_t pin;
        int level;
    };
    // Safe levels first (rail and resets held low, amplifier muted).
    const PinInit pins[] = {
        {kIoePinMuxCtrl, 0},     // same value as the stock firmware (external port mux)
        {kIoePinAudioEn, 0},     // audio rail unused in this build
        {kIoePinTouchRst, 0},
        {kIoePinOledRst, 0},
        {kIoePinL3bEn, 0},
        {kIoePinSpeakerPa, 0},
    };

    for (const auto &p : pins) {
        if (!ioe_set_level(p.pin, p.level)) {
            return false;
        }
    }
    for (const auto &p : pins) {
        if (!ioe_set_push_pull(p.pin) || !ioe_set_output(p.pin)) {
            return false;
        }
    }
    return true;
}

bool ioe_enable_l3b()
{
    if (!ioe_set_level(kIoePinL3bEn, 1)) {
        return false;
    }

    for (int attempt = 0; attempt < 10; ++attempt) {
        if (ioe_read_pin(kIoePinL3bEn)) {
            ESP_LOGI(TAG, "3V3_L3B enabled (L3B_EN readback high)");
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    ESP_LOGE(TAG, "3V3_L3B did not come up (L3B_EN readback still low)");
    return false;
}

void ioe_pulse_reset(uint8_t pin, const char *name, uint32_t settle_ms)
{
    ioe_set_level(pin, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    ioe_set_level(pin, 1);
    vTaskDelay(pdMS_TO_TICKS(settle_ms));
    ESP_LOGI(TAG, "%s reset released, waited %ums", name, (unsigned)settle_ms);
}

// ------------------------------------------------------------- CO5300 panel
class PanelCO5300 : public lgfx::Panel_AMOLED {
public:
    PanelCO5300()
    {
        _cfg.memory_width = 480;
        _cfg.memory_height = 480;
        _write_depth = lgfx::rgb565_2Byte;
        _read_depth = lgfx::rgb565_2Byte;
    }

    const uint8_t *getInitCommands(uint8_t listno) const override
    {
        // M5GFX 0.2.19 / M5StopWatch-UserDemo 0.5 (MIT).
        static constexpr uint8_t list0[] = {
            0x11, 0 + CMD_INIT_DELAY, 150,  // sleep out
            0xC4, 1, 0x80,                  // set SPI mode
            0x35, 1, 0x80,                  // TE on
            0x44, 2, 0x01, 0xD2,            // TE scanline = 466
            0x53, 1, 0x20,                  // brightness block on
            0x36, 1, 0x00,                  // MADCTL
            0x51, 1, 0xA0,                  // brightness
            0x29, 0,                        // display on
            0xFF, 0xFF,
        };
        return listno == 0 ? list0 : nullptr;
    }
};

class NexDisplay : public lgfx::LGFX_Device {
public:
    NexDisplay()
    {
        {
            auto bus = _bus.config();
            bus.freq_write = 80000000;
            bus.freq_read = 10000000;
            bus.pin_sclk = kPinSclk;
            bus.pin_io0 = kPinIo0;
            bus.pin_io1 = kPinIo1;
            bus.pin_io2 = kPinIo2;
            bus.pin_io3 = kPinIo3;
            bus.spi_host = SPI2_HOST;
            bus.spi_mode = 0;
            bus.spi_3wire = true;
            bus.dma_channel = SPI_DMA_CH_AUTO;
            _bus.config(bus);
            _panel.setBus(&_bus);
        }
        {
            auto panel = _panel.config();
            panel.pin_rst = GPIO_NUM_NC;
            panel.pin_cs = kPinCs;
            panel.panel_width = kPanelWindowW;
            panel.panel_height = kPanelWindowH;
            panel.offset_x = kPanelOffsetX;
            panel.offset_y = 0;
            panel.readable = false;
            _panel.config(panel);
        }
        setPanel(&_panel);
    }

    bool begin()
    {
        // Hardware reset is driven through the M5IOE1, so M5GFX must not
        // try to toggle a reset pin of its own.
        if (!init_without_reset(false)) {
            return false;
        }
        lgfx::pinMode(kPinTe, lgfx::pin_mode_t::input_pullup);

        // Mirror the official STOPWATCH path: render into an AMOLED
        // framebuffer in PSRAM and let M5GFX push it row by row through its
        // internal DMA buffers. Writing the QSPI panel directly from a PSRAM
        // buffer is unsafe here: the SPI DMA engine bypasses the cache, so
        // freshly rendered pixels may not be visible to it yet.
        if (!_panel.initPanelFb()) {
            ESP_LOGE(TAG, "AMOLED framebuffer allocation failed");
            return false;
        }
        auto *fb = _panel.getPanelFb();
        if (fb == nullptr) {
            ESP_LOGE(TAG, "AMOLED framebuffer unavailable");
            return false;
        }
        fb->setBus(&_bus);
        fb->setAutoDisplay(true);
        setPanel(fb);
        ESP_LOGI(TAG, "AMOLED framebuffer enabled (%ux%u, %u bytes PSRAM)",
                 (unsigned)kPanelWindowW, (unsigned)kPanelWindowH,
                 (unsigned)(kPanelWindowW * kPanelWindowH * sizeof(uint16_t)));

        // The active panel is the framebuffer now, so drive the real panel's
        // brightness register explicitly.
        _panel.setBrightness(160);
        return true;
    }

private:
    lgfx::Bus_SPI _bus;
    PanelCO5300 _panel;
};

NexDisplay s_display;
bool s_display_ok = false;

// ------------------------------------------------------------------ CST820
bool touch_init()
{
    s_touch = i2c_add_device(kTouchAddr);
    if (s_touch == nullptr) {
        ESP_LOGE(TAG, "CST820 not present at 0x%02X", kTouchAddr);
        return false;
    }

    uint8_t chip_id = 0;
    uint8_t soft_ver = 0;
    if (!i2c_read_regs(s_touch, 0xA7, &chip_id, 1) || !i2c_read_regs(s_touch, 0xA9, &soft_ver, 1)) {
        ESP_LOGE(TAG, "CST820 register read failed");
        return false;
    }
    ESP_LOGI(TAG, "CST820 found at 0x%02X: chip=0x%02X fw=0x%02X", kTouchAddr, chip_id, soft_ver);
    if (chip_id == 0x00 || chip_id == 0xFF) {
        ESP_LOGE(TAG, "CST820 returned an invalid chip id");
        return false;
    }
    s_touch_ok = true;
    return true;
}

}  // namespace

bool board_speaker_enable(bool enable)
{
    if (s_ioe_addr == 0) {
        return false;
    }
    // IO3: audio enable (powers ES8311), IO10: speaker PA.
    ioe_set_output(2);
    ioe_set_push_pull(2);
    ioe_set_level(2, 1);
    ioe_set_output(9);
    ioe_set_push_pull(9);
    ioe_set_level(9, enable ? 1 : 0);
    gpio_config_t config = {};
    config.pin_bit_mask = 1ULL << 14;
    config.mode = GPIO_MODE_OUTPUT;
    config.pull_up_en = GPIO_PULLUP_DISABLE;
    config.pull_down_en = GPIO_PULLDOWN_DISABLE;
    config.intr_type = GPIO_INTR_DISABLE;
    gpio_config(&config);
    gpio_set_level(GPIO_NUM_14, enable ? 1 : 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    return true;
}

bool board_init()
{
    if (!i2c_init()) {
        return false;
    }

    // PMIC failure is reported but not fatal: the rails are already on at
    // power-up, and the IOE probe below decides whether the board is usable.
    pmic_init();

    if (!ioe_probe()) {
        ESP_LOGE(TAG, "M5IOE1 not found; cannot power the AMOLED rail");
        return false;
    }
    ESP_LOGI(TAG, "M5IOE1 address in use: 0x%02X", s_ioe_addr);

    if (!ioe_setup_outputs()) {
        ESP_LOGE(TAG, "M5IOE1 output configuration failed");
        return false;
    }
    if (!ioe_enable_l3b()) {
        return false;
    }

    ioe_pulse_reset(kIoePinOledRst, "OLED", 120);
    ioe_pulse_reset(kIoePinTouchRst, "touch", 100);

    if (!s_display.begin()) {
        ESP_LOGE(TAG, "CO5300 panel init failed");
        return false;
    }
    s_display_ok = true;
    ESP_LOGI(TAG, "CO5300 ready: %dx%d window, offset x=%d, QSPI @80MHz",
             (int)s_display.width(), (int)s_display.height(), (int)kPanelOffsetX);

    if (!touch_init()) {
        ESP_LOGW(TAG, "touch unavailable; UI will start without touch");
    }
    return true;
}

lgfx::LGFX_Device &board_display()
{
    return s_display;
}

i2c_master_bus_handle_t board_i2c_bus()
{
    return s_bus;
}

bool board_touch_init()
{
    return s_touch_ok;
}

bool board_touch_read(uint16_t &x, uint16_t &y, bool &pressed)
{
    if (!s_touch_ok || s_touch == nullptr) {
        return false;
    }

    // 0x00: status, 0x02: finger count, 0x03/0x04: X, 0x05/0x06: Y.
    uint8_t buf[7] = {};
    if (!i2c_read_regs(s_touch, 0x00, buf, sizeof(buf))) {
        return false;
    }

    const uint8_t fingers = buf[2];
    const uint8_t event = (buf[3] & 0xC0) >> 6;  // 0 down, 1 up, 2 contact
    uint16_t raw_x = static_cast<uint16_t>(((buf[3] & 0x0F) << 8) | buf[4]);
    uint16_t raw_y = static_cast<uint16_t>(((buf[5] & 0x0F) << 8) | buf[6]);

    static bool range_logged = false;
    if (!range_logged && (raw_x > kPanelWindowW - 1 || raw_y > kPanelWindowH - 1)) {
        ESP_LOGW(TAG, "touch raw out of window: x=%u y=%u", raw_x, raw_y);
        range_logged = true;
    }

    x = raw_x < kPanelWindowW ? raw_x : kPanelWindowW - 1;
    y = raw_y < kPanelWindowH ? raw_y : kPanelWindowH - 1;
    pressed = fingers > 0 && (event == 0 || event == 2);
    return true;
}

}  // namespace nex
