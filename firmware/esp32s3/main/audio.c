/**
 * @file audio.c
 * @brief I2S + ES8311 audio driver.
 *
 * Hardware (vendor BSP):
 *   I2S: MCLK=GPIO4, BCLK=GPIO5, WS=GPIO7, DOUT=GPIO8 (ESP->codec), DIN=GPIO6 (codec->ESP)
 *   ES8311 @ I2C 0x18, PA enable GPIO1 (active low)
 *   16 kHz, 16-bit stereo, MCLK = 384 * fs
 */
#include "audio.h"

#include <math.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"

#include "es8311.h"

static const char *TAG = "AUDIO";

#define PIN_PA_ENABLE  1
#define PIN_MCLK       4
#define PIN_BCLK       5
#define PIN_WS         7
#define PIN_DOUT       8
#define PIN_DIN        6
#define I2S_PORT       I2S_NUM_1
#define SAMPLE_RATE    16000
#define MAX_FRAMES     512

esp_err_t es8311_codec_init(void);
esp_err_t es8311_codec_set_volume(int volume);

static i2s_chan_handle_t s_tx;
static i2s_chan_handle_t s_rx;
static bool s_inited;
static int16_t s_tmp[2 * MAX_FRAMES];
static int16_t s_tmp_in[2 * MAX_FRAMES];

esp_err_t audio_init(void)
{
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << PIN_PA_ENABLE,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&io), TAG, "pa gpio config failed");
    gpio_set_level(PIN_PA_ENABLE, 0);

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_PORT, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &s_tx, &s_rx), TAG, "i2s channel failed");

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                       I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = PIN_MCLK,
            .bclk = PIN_BCLK,
            .ws = PIN_WS,
            .dout = PIN_DOUT,
            .din = PIN_DIN,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    std_cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_384;

    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_tx, &std_cfg), TAG, "i2s tx init failed");
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_rx, &std_cfg), TAG, "i2s rx init failed");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_tx), TAG, "i2s tx enable failed");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_rx), TAG, "i2s rx enable failed");

    esp_err_t err = es8311_codec_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ES8311 init failed: %s", esp_err_to_name(err));
        return err;
    }
    s_inited = true;
    ESP_LOGI(TAG, "audio ready: 16kHz/16bit, ES8311 @0x18, PA GPIO%d", PIN_PA_ENABLE);
    audio_play_beep();
    return ESP_OK;
}

void audio_play_beep(void)
{
    if (!s_inited) {
        return;
    }
    size_t written = 0;
    const int total = SAMPLE_RATE / 5; /* 200 ms */
    for (int done = 0; done < total; done += 512) {
        int n = (total - done > 512) ? 512 : (total - done);
        for (int i = 0; i < n; i++) {
            float t = (float)(done + i) / SAMPLE_RATE;
            int16_t v = (int16_t)(sinf(2.0f * (float)M_PI * 1000.0f * t) * 6000);
            s_tmp[i * 2] = v;
            s_tmp[i * 2 + 1] = v;
        }
        i2s_channel_write(s_tx, s_tmp, (size_t)n * 4, &written, 1000);
    }
}

esp_err_t audio_read_mic(int16_t *mono, int frames)
{
    if (!s_inited || frames <= 0 || frames > MAX_FRAMES) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t bytes_read = 0;
    size_t want = (size_t)frames * 4; /* stereo 16-bit */
    esp_err_t err = i2s_channel_read(s_rx, s_tmp_in, want, &bytes_read, 1000);
    if (err != ESP_OK) {
        return err;
    }
    int got = (int)(bytes_read / 4);
    for (int i = 0; i < got; i++) {
        int32_t l = s_tmp_in[i * 2];
        int32_t r = s_tmp_in[i * 2 + 1];
        mono[i] = (int16_t)((l + r) / 2);
    }
    for (int i = got; i < frames; i++) {
        mono[i] = 0;
    }
    return ESP_OK;
}

esp_err_t audio_write_speaker(const int16_t *mono, int frames)
{
    if (!s_inited || frames <= 0 || frames > MAX_FRAMES) {
        return ESP_ERR_INVALID_ARG;
    }
    for (int i = 0; i < frames; i++) {
        s_tmp[i * 2] = mono[i];
        s_tmp[i * 2 + 1] = mono[i];
    }
    size_t written = 0;
    return i2s_channel_write(s_tx, s_tmp, (size_t)frames * 4, &written, 1000);
}


void audio_set_volume(int percent)
{
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    esp_err_t err = es8311_codec_set_volume(percent);
    ESP_LOGI(TAG, "volume -> %d%% (%s)", percent, esp_err_to_name(err));
}
