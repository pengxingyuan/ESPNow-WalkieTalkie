#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"

#include "lvgl.h"
#include "lv_port_disp.h"
#include "lv_port_indev.h"

#include "app_ui.h"
#include "net_link.h"
#include "power.h"
#include "audio.h"
#include "voice.h"

#include "driver/i2c.h"

static const char *TAG = "MAIN";

/* 一次性 I2C 扫描：找触摸/音频 codec 的地址（语音 bring-up 用） */
static void i2c_scan_bus(void)
{
    ESP_LOGI(TAG, "I2C scan on port 0...");
    for (uint8_t addr = 0x08; addr < 0x78; addr++) {
        i2c_cmd_handle_t cmd = i2c_cmd_link_create();
        i2c_master_start(cmd);
        i2c_master_write_byte(cmd, (addr << 1) | I2C_MASTER_WRITE, true);
        i2c_master_stop(cmd);
        esp_err_t err = i2c_master_cmd_begin(I2C_NUM_0, cmd, pdMS_TO_TICKS(20));
        i2c_cmd_link_delete(cmd);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "I2C device found: 0x%02X", addr);
        }
    }
    ESP_LOGI(TAG, "I2C scan done");
}


/* 开机自检：确认界面用到的汉字在字库里都存在（缺字会显示成方框） */
static void inc_lvgl_tick(void *arg)
{
    LV_UNUSED(arg);
    lv_tick_inc(10);
}

/* ---- UI 事件回调：文本走 ESP-NOW，PTT 暂只打日志 ---- */
static void on_send_text(const char *text)
{
    ESP_LOGI(TAG, "UI send text: %s", text);
    net_link_send_text(text);
}

static void on_ptt_start(void)
{
    ESP_LOGI(TAG, "PTT start (TALK)");
    voice_ptt_start();
}

static void on_ptt_stop(void)
{
    ESP_LOGI(TAG, "PTT stop (release)");
    voice_ptt_stop();
}

static void on_pair_request(const char *peer_name)
{
    ESP_LOGI(TAG, "Pair request: %s", peer_name ? peer_name : "?");
}

static void on_volume_set(int percent)
{
    audio_set_volume(percent);
    audio_play_beep(); /* audible feedback while adjusting volume */
}

static void on_brightness_set(int percent)
{
    power_set_backlight(percent);
}

static void on_name_set(const char *name)
{
    ESP_LOGI(TAG, "UI set node name: %s", name);
    net_link_set_name(name);
    ui_app_set_name(name);
}

static void on_scan_request(void)
{
    ESP_LOGI(TAG, "Scan request");
    net_link_send_hello();
}

static const ui_callbacks_t s_ui_cbs = {
    .on_send_text = on_send_text,
    .on_ptt_start = on_ptt_start,
    .on_ptt_stop = on_ptt_stop,
    .on_pair_request = on_pair_request,
    .on_scan_request = on_scan_request,
    .on_name_set = on_name_set,
    .on_volume_set = on_volume_set,
    .on_brightness_set = on_brightness_set,
};

void app_main(void)
{
    lv_init();

    lv_port_disp_init();
    lv_port_indev_init();
    i2c_scan_bus();

    /* 为 LVGL 提供时基 */
    const esp_timer_create_args_t lvgl_tick_timer_args = {
        .callback = &inc_lvgl_tick,
        .name = "lvgl_tick",
    };
    esp_timer_handle_t lvgl_tick_timer = NULL;
    ESP_ERROR_CHECK(esp_timer_create(&lvgl_tick_timer_args, &lvgl_tick_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(lvgl_tick_timer, 10 * 1000));

    /* 构建对讲机界面 */
    ui_app_init();
    ui_app_set_callbacks(&s_ui_cbs);
    power_init();
    if (audio_init() == ESP_OK) {
        voice_init();
    }

    /* ESP-NOW 链路 */
    net_link_init();
    ui_app_set_name(net_link_get_name());
    char hello[48];
    snprintf(hello, sizeof(hello), "%s | CH 6", net_link_get_name());
    ui_app_add_system_message(hello);

    ESP_LOGI(TAG, "app started");

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10));
        power_process();
        net_link_process(); /* 把收到的消息刷新到 LVGL（UI 线程） */
        lv_task_handler();
    }
}
