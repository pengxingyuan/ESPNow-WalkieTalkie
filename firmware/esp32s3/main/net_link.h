/**
 * @file net_link.h
 * @brief ESP-NOW 点对点链路（文本消息最小闭环）
 *
 * 架构：
 *   - WiFi STA 模式 + 固定信道（ESP-NOW 双方必须同信道）
 *   - 广播发现（HELLO）+ 文本消息（TEXT）
 *   - 接收回调里只做组包，通过 FreeRTOS 队列交给 LVGL 线程（net_link_process）
 *
 * 依赖：需先调用 ui_app_init()，再调用 net_link_init()。
 */
#ifndef NET_LINK_H
#define NET_LINK_H

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 初始化 NVS / WiFi / ESP-NOW，并广播 HELLO */
void net_link_init(void);

/** 在 LVGL 主循环里调用：把收到的消息更新到界面 */
void net_link_process(void);

/** 发送一条文本（广播） */
void net_link_send_text(const char *text);

/** Send a voice payload (ESP-NOW broadcast) */
esp_err_t net_link_send_voice(const uint8_t *payload, uint16_t len);

/** 立即广播一次 HELLO（配对页“扫描/刷新”用） */
void net_link_send_hello(void);

/** Set local node name (stored in NVS + re-announced) */
void net_link_set_name(const char *name);

/** 本机名称，例如 "对讲机-3F2A" */
const char *net_link_get_name(void);

#ifdef __cplusplus
}
#endif

#endif /* NET_LINK_H */
