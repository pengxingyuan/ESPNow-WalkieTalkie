/**
 * @file app_ui.h
 * @brief Walkie-talkie UI (LVGL 8.4, portrait 240x320, black/neon-green)
 */
#ifndef APP_UI_H
#define APP_UI_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    UI_LINK_DISCONNECTED = 0,
    UI_LINK_SEARCHING,
    UI_LINK_CONNECTED,
} ui_link_state_t;

typedef struct {
    void (*on_send_text)(const char *text);
    void (*on_ptt_start)(void);
    void (*on_ptt_stop)(void);
    void (*on_pair_request)(const char *peer_name);
    void (*on_scan_request)(void);
    /** user edited the local node name (settings / NODES page) */
    void (*on_name_set)(const char *name);
    /** speaker volume percent 0..100 */
    void (*on_volume_set)(int percent);
    /** backlight brightness percent 0..100 */
    void (*on_brightness_set)(int percent);
} ui_callbacks_t;

void ui_app_init(void);
void ui_app_set_callbacks(const ui_callbacks_t *cbs);
void ui_app_set_link(ui_link_state_t state, const char *peer_name);
void ui_app_set_battery(uint8_t percent);
void ui_app_add_message(const char *text, bool outgoing);
/** incoming message with sender label prefix */
void ui_app_add_message_from(const char *sender, const char *text);
void ui_app_add_system_message(const char *text);
void ui_app_set_voice_level(uint8_t level);
void ui_app_set_name(const char *name);

typedef struct {
    char name[24];
    int8_t rssi;
} ui_peer_t;

void ui_app_set_peers(const ui_peer_t *peers, int count);

#ifdef __cplusplus
}
#endif

#endif /* APP_UI_H */
