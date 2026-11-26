/*
 * NEX firmware — ES8311 speaker output
 * SPDX-License-Identifier: MIT
 *
 * I2S pins confirmed from the M5Stack StopWatch user demo:
 * MCLK 18, BCLK 17, LRCK 15, DAC out 21. The AW8737A amplifier has no
 * controllable enable pin on this board.
 */
#include "audio.h"

#include <cmath>
#include <cstring>

#include "board.h"
#include "driver/i2s_std.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_log.h"
#include "es8311_codec.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace nex {
namespace {

constexpr const char *TAG = "NEX-AUDIO";
constexpr int kSampleRate = 44100;
constexpr int kChunkSamples = 1024;

i2s_chan_handle_t s_tx = nullptr;
const audio_codec_data_if_t *s_data_if = nullptr;
const audio_codec_ctrl_if_t *s_ctrl_if = nullptr;
const audio_codec_gpio_if_t *s_gpio_if = nullptr;
const audio_codec_if_t *s_codec_if = nullptr;
esp_codec_dev_handle_t s_dev = nullptr;
volatile bool s_active = false;

void beep_task(void *)
{
    static int16_t buffer[kChunkSamples];
    constexpr float kTwoPi = 6.2831853f;
    float phase = 0.0f;
    const float step = kTwoPi * 880.0f / kSampleRate;

    while (s_active) {
        for (int beep = 0; beep < 3 && s_active; ++beep) {
            int remaining = kSampleRate * 180 / 1000;
            while (remaining > 0 && s_active) {
                const int count = remaining < kChunkSamples ? remaining : kChunkSamples;
                for (int i = 0; i < count; ++i) {
                    buffer[i] = static_cast<int16_t>(std::sin(phase) * 20000.0f);
                    phase += step;
                    if (phase > kTwoPi) {
                        phase -= kTwoPi;
                    }
                }
                esp_codec_dev_write(s_dev, buffer, count * static_cast<int>(sizeof(int16_t)));
                remaining -= count;
            }
            if (!s_active) {
                break;
            }
            std::memset(buffer, 0, sizeof(buffer));
            for (int i = 0; i < 4; ++i) {
                esp_codec_dev_write(s_dev, buffer, sizeof(buffer));
            }
        }
    }
    s_active = false;
    vTaskDelete(nullptr);
}

}  // namespace

bool audio_init()
{
    if (s_dev != nullptr) {
        return true;
    }

    board_speaker_enable(true);

    i2s_chan_config_t channel_config = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    channel_config.auto_clear = true;
    if (i2s_new_channel(&channel_config, &s_tx, nullptr) != ESP_OK) {
        ESP_LOGE(TAG, "i2s_new_channel failed");
        return false;
    }

    i2s_std_config_t std_config = {};
    std_config.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(kSampleRate);
    std_config.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    std_config.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO);
    std_config.gpio_cfg.mclk = GPIO_NUM_18;
    std_config.gpio_cfg.bclk = GPIO_NUM_17;
    std_config.gpio_cfg.ws = GPIO_NUM_15;
    std_config.gpio_cfg.dout = GPIO_NUM_21;
    std_config.gpio_cfg.din = I2S_GPIO_UNUSED;
    if (i2s_channel_init_std_mode(s_tx, &std_config) != ESP_OK ||
        i2s_channel_enable(s_tx) != ESP_OK) {
        ESP_LOGE(TAG, "i2s init failed");
        return false;
    }

    audio_codec_i2s_cfg_t i2s_config = {};
    i2s_config.port = I2S_NUM_0;
    i2s_config.tx_handle = s_tx;
    s_data_if = audio_codec_new_i2s_data(&i2s_config);

    audio_codec_i2c_cfg_t i2c_config = {};
    i2c_config.addr = ES8311_CODEC_DEFAULT_ADDR;
    i2c_config.bus_handle = board_i2c_bus();
    i2c_config.clock_speed_hz = 100000;
    s_ctrl_if = audio_codec_new_i2c_ctrl(&i2c_config);
    s_gpio_if = audio_codec_new_gpio();

    es8311_codec_cfg_t codec_config = {};
    codec_config.ctrl_if = s_ctrl_if;
    codec_config.gpio_if = s_gpio_if;
    codec_config.codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC;
    codec_config.pa_pin = GPIO_NUM_NC;
    codec_config.master_mode = false;
    codec_config.use_mclk = true;
    codec_config.hw_gain.pa_voltage = 5.0f;
    codec_config.hw_gain.codec_dac_voltage = 3.3f;
    codec_config.hw_gain.pa_gain = 0.0f;
    s_codec_if = es8311_codec_new(&codec_config);

    esp_codec_dev_cfg_t dev_config = {};
    dev_config.codec_if = s_codec_if;
    dev_config.data_if = s_data_if;
    dev_config.dev_type = ESP_CODEC_DEV_TYPE_OUT;
    s_dev = esp_codec_dev_new(&dev_config);

    if (s_data_if == nullptr || s_ctrl_if == nullptr || s_codec_if == nullptr || s_dev == nullptr) {
        ESP_LOGE(TAG, "codec init failed");
        s_dev = nullptr;
        return false;
    }

    esp_codec_dev_sample_info_t sample = {};
    sample.bits_per_sample = 16;
    sample.channel = 1;
    sample.sample_rate = kSampleRate;
    esp_codec_dev_set_out_vol(s_dev, 100);
    if (esp_codec_dev_open(s_dev, &sample) != ESP_OK) {
        ESP_LOGE(TAG, "codec open failed");
        return false;
    }
    ESP_LOGI(TAG, "ES8311 speaker ready");
    return true;
}

void audio_alarm_start()
{
    if (s_dev == nullptr || s_active) {
        return;
    }
    s_active = true;
    if (xTaskCreate(beep_task, "nex_beep", 4 * 1024, nullptr, 4, nullptr) != pdPASS) {
        s_active = false;
    }
}

void audio_alarm_stop()
{
    s_active = false;
}

}  // namespace nex
