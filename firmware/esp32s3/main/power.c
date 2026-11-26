/**
 * @file power.c
 * @brief Power management for the 2.8" ESP32-S3 board.
 *
 *  - Backlight: GPIO45 driven by LEDC (PWM mode enabled in sdkconfig).
 *    The vendor display driver already configures LEDC timer0/ch0 (10-bit, 5kHz),
 *    so we only update the duty cycle here.
 *  - Battery: GPIO9 = ADC1 channel 8, through the on-board voltage divider.
 *    DIVIDER_RATIO is a guess (1:2). Measure the real board with a multimeter
 *    and adjust if the reported voltage is off.
 */
#include "power.h"

#include <stdio.h>

#include "driver/ledc.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "app_ui.h"

static const char *TAG = "POWER";

#define BACKLIGHT_GPIO         45
#define BACKLIGHT_LEDC_CH      LEDC_CHANNEL_0
#define BACKLIGHT_LEDC_TIMER   LEDC_TIMER_0
#define BACKLIGHT_DEFAULT_PCT  70

#define BAT_ADC_UNIT           ADC_UNIT_1
#define BAT_ADC_CHANNEL        ADC_CHANNEL_8 /* GPIO9 */
#define BAT_ADC_ATTEN          ADC_ATTEN_DB_12
#define DIVIDER_RATIO          2.0f          /* battery mV = pin mV * ratio */
#define SAMPLE_PERIOD_MS       5000

static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t s_cali;
static int s_battery_mv;
static int64_t s_last_sample_ms;

/* ---------------- backlight ---------------- */
void power_set_backlight(int percent)
{
    if (percent < 5) {
        percent = 5;
    }
    if (percent > 100) {
        percent = 100;
    }
    uint32_t duty = (uint32_t)(1023 * percent / 100);
    ledc_set_duty(LEDC_LOW_SPEED_MODE, BACKLIGHT_LEDC_CH, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, BACKLIGHT_LEDC_CH);
    ESP_LOGI(TAG, "backlight: %d%%", percent);
}

/* ---------------- battery ---------------- */
static int battery_percent_from_mv(int mv)
{
    /* Li-ion rest-voltage curve, linear between points */
    static const struct {
        int mv;
        int pct;
    } curve[] = {
        {4150, 100}, {4050, 90}, {3950, 80}, {3850, 65},
        {3750, 50},  {3650, 35}, {3550, 20}, {3450, 10}, {3300, 0},
    };
    const int n = (int)(sizeof(curve) / sizeof(curve[0]));
    if (mv >= curve[0].mv) {
        return 100;
    }
    if (mv <= curve[n - 1].mv) {
        return 0;
    }
    for (int i = 0; i < n - 1; i++) {
        if (mv <= curve[i].mv && mv >= curve[i + 1].mv) {
            int dv = curve[i].mv - curve[i + 1].mv;
            int dp = curve[i].pct - curve[i + 1].pct;
            return curve[i + 1].pct + (mv - curve[i + 1].mv) * dp / dv;
        }
    }
    return 0;
}

static int sample_battery_mv(void)
{
    if (s_adc == NULL) {
        return 0;
    }

    int raw_sum = 0;
    int ok = 0;
    for (int i = 0; i < 8; i++) {
        int raw = 0;
        if (adc_oneshot_read(s_adc, BAT_ADC_CHANNEL, &raw) == ESP_OK) {
            raw_sum += raw;
            ok++;
        }
    }
    if (ok == 0) {
        return 0;
    }
    int raw = raw_sum / ok;

    int pin_mv = 0;
    if (s_cali != NULL) {
        adc_cali_raw_to_voltage(s_cali, raw, &pin_mv);
    } else {
        /* rough fallback: 12 dB attenuation ~ 3.1 V full scale */
        pin_mv = raw * 3100 / 4095;
    }

    int batt_mv = (int)(pin_mv * DIVIDER_RATIO);
    s_battery_mv = batt_mv;
    return batt_mv;
}

/* ---------------- public ---------------- */
void power_init(void)
{
    /* backlight PWM was set up by the display driver; just dim it */
    power_set_backlight(BACKLIGHT_DEFAULT_PCT);

    adc_oneshot_unit_init_cfg_t init_cfg = {
        .unit_id = BAT_ADC_UNIT,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    if (adc_oneshot_new_unit(&init_cfg, &s_adc) != ESP_OK) {
        ESP_LOGW(TAG, "battery ADC init failed");
        return;
    }
    adc_oneshot_chan_cfg_t chan_cfg = {
        .atten = BAT_ADC_ATTEN,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    if (adc_oneshot_config_channel(s_adc, BAT_ADC_CHANNEL, &chan_cfg) != ESP_OK) {
        ESP_LOGW(TAG, "battery ADC channel config failed");
    }

#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    adc_cali_curve_fitting_config_t cali_cfg = {
        .unit_id = BAT_ADC_UNIT,
        .chan = BAT_ADC_CHANNEL,
        .atten = BAT_ADC_ATTEN,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    if (adc_cali_create_scheme_curve_fitting(&cali_cfg, &s_cali) != ESP_OK) {
        s_cali = NULL;
    }
#endif

    s_last_sample_ms = esp_timer_get_time() / 1000 - SAMPLE_PERIOD_MS;
    int mv = sample_battery_mv();
    int pct = battery_percent_from_mv(mv);
    ESP_LOGI(TAG, "battery: %d mV (%d%%)", mv, pct);
    ui_app_set_battery((uint8_t)pct);
}

void power_process(void)
{
    int64_t now = esp_timer_get_time() / 1000;
    if (now - s_last_sample_ms < SAMPLE_PERIOD_MS) {
        return;
    }
    s_last_sample_ms = now;
    int mv = sample_battery_mv();
    if (mv == 0) {
        return;
    }
    int pct = battery_percent_from_mv(mv);
    ESP_LOGD(TAG, "battery: %d mV (%d%%)", mv, pct);
    ui_app_set_battery((uint8_t)pct);
}

int power_get_battery_mv(void)
{
    return s_battery_mv;
}
