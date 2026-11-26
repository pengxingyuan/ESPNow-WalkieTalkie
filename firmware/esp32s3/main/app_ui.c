/**
 * @file app_ui.c
 * @brief Walkie-talkie UI (LVGL 8.4, portrait 240x320, black + neon-green theme)
 *
 * Layout:
 *   ┌──────────────────────┐ 0
 *   │ [gear] ● NO LINK [bat]85%│ 28  status bar (1px neon-green hairline on top)
 *   ├──────────────────────┤
 *   │     bubble list      │       chat area
 *   ├──────────────────────┤ 250
 *   │ ┌────┐  [OK]  [HI]   │
 *   │ │HOLD│  [COMING][TEXT]│ 320  bottom bar
 *   │ │TALK│                │
 *   └──────┘───────────────┘
 *
 * Style: pure black + neon green (Great Scott Gadgets homepage vibe).
 * Font: LVGL built-in Montserrat 16 (includes LV_SYMBOL_* icons).
 */
#include "app_ui.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "lvgl.h"

#include "lv_port_disp.h"
#include "lv_port_indev.h"

static const char *TAG = "APP_UI";

/* ---------------- size (portrait) ---------------- */
#define SCR_W           240
#define SCR_H           320
#define STATUS_H        34
#define BOTTOM_H        70
#define CHAT_H          (SCR_H - STATUS_H - BOTTOM_H)
#define BUBBLE_MAX_W    196
#define LABEL_MAX_W     176
#define MAX_MESSAGES    40

/* ---------------- palette: pure black + neon green ---------------- */
#define COL_BG          0x000000
#define COL_BAR         0x080808
#define COL_LINE        0x242424
#define COL_TEXT        0xF2F2F2
#define COL_MUTED       0x8A8A8A
#define COL_ACCENT      0x39FF14
#define COL_WARN        0xFFC53D
#define COL_DANGER      0xFF4D4D
#define COL_IN_BUBBLE   0x141414
#define COL_IN_BORDER   0x2E2E2E
#define COL_OUT_BUBBLE  0x39FF14

/* ---------------- state ---------------- */
static const lv_font_t *ui_font;
static ui_callbacks_t s_cbs;
static ui_link_state_t s_link = UI_LINK_DISCONNECTED;
static uint8_t s_battery = 85;
static uint8_t s_voice_level = 0;
static int s_peer_count = 0;
static uint32_t s_msg_count = 0;

/* main screen */
static lv_obj_t *scr_main;
static lv_obj_t *chat_list;
static lv_obj_t *link_dot;
static lv_obj_t *link_label;
static lv_obj_t *battery_icon;
static lv_obj_t *battery_label;
static lv_obj_t *ptt_btn;
static lv_obj_t *ptt_label;

/* pairing screen */
static lv_obj_t *scr_pair;
static lv_obj_t *scr_settings;
static lv_obj_t *vol_label;
static lv_obj_t *bri_label;
static int s_volume = 85;
static int s_brightness = 70;
static lv_obj_t *pair_peer_list;

/* landscape typing screen */
static lv_obj_t *scr_typing;
static lv_obj_t *typing_ta;
static lv_obj_t *s_prev_scr;
static lv_obj_t *settings_name_label;
static lv_obj_t *own_name_label;
static char s_my_name[24] = "NODE";
static int kb_mode; /* 0=message, 1=name */

/* ---------------- helpers ---------------- */
static void style_reset(lv_obj_t *obj)
{
    lv_obj_remove_style_all(obj);
}

static lv_obj_t *make_label(lv_obj_t *parent, const char *text, lv_color_t color,
                            const lv_font_t *font)
{
    lv_obj_t *lab = lv_label_create(parent);
    lv_label_set_text(lab, text);
    lv_obj_set_style_text_color(lab, color, 0);
    lv_obj_set_style_text_font(lab, font ? font : ui_font, 0);
    return lab;
}

static lv_obj_t *make_box(lv_obj_t *parent, lv_coord_t x, lv_coord_t y,
                          lv_coord_t w, lv_coord_t h, lv_coord_t radius,
                          lv_color_t bg, lv_opa_t bg_opa,
                          lv_color_t border, lv_coord_t border_w)
{
    lv_obj_t *obj = lv_obj_create(parent);
    style_reset(obj);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, w, h);
    lv_obj_set_style_radius(obj, radius, 0);
    lv_obj_set_style_bg_color(obj, bg, 0);
    lv_obj_set_style_bg_opa(obj, bg_opa, 0);
    lv_obj_set_style_border_width(obj, border_w, 0);
    lv_obj_set_style_border_color(obj, border, 0);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    return obj;
}

/* ---------------- main screen ---------------- */
static void link_update_visual(void)
{
    lv_color_t dot;
    switch (s_link) {
    case UI_LINK_CONNECTED:
        dot = lv_color_hex(COL_ACCENT);
        break;
    case UI_LINK_SEARCHING:
        dot = lv_color_hex(COL_WARN);
        break;
    default:
        dot = lv_color_hex(COL_MUTED);
        break;
    }
    lv_obj_set_style_bg_color(link_dot, dot, 0);
}

static void settings_btn_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    lv_scr_load_anim(scr_settings, LV_SCR_LOAD_ANIM_MOVE_LEFT, 180, 0, false);
}

static void ptt_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);

    if (code == LV_EVENT_PRESSED) {
        lv_label_set_text(ptt_label, "ON\nAIR");
        lv_obj_set_style_bg_opa(ptt_btn, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(ptt_btn, lv_color_hex(COL_ACCENT), 0);
        lv_obj_set_style_border_color(ptt_btn, lv_color_hex(COL_ACCENT), 0);
        lv_obj_set_style_text_color(ptt_label, lv_color_hex(0x000000), 0);
        if (s_cbs.on_ptt_start) {
            s_cbs.on_ptt_start();
        }
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        lv_label_set_text(ptt_label, "HOLD\nTALK");
        lv_obj_set_style_bg_opa(ptt_btn, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_color(ptt_btn, lv_color_hex(COL_ACCENT), 0);
        lv_obj_set_style_text_color(ptt_label, lv_color_hex(COL_TEXT), 0);
        if (s_cbs.on_ptt_stop) {
            s_cbs.on_ptt_stop();
        }
    }
}

static void send_text_internal(const char *text)
{
    if (!text || text[0] == '\0') {
        return;
    }
    ui_app_add_message(text, true);
    if (s_cbs.on_send_text) {
        s_cbs.on_send_text(text);
    }
}

static void chip_click_cb(lv_event_t *e)
{
    const char *text = (const char *)lv_event_get_user_data(e);
    send_text_internal(text);
}

/* ---------------- landscape typing screen ---------------- */
static void build_typing_screen(void);

static void typing_close(void)
{
    lv_port_disp_set_landscape(false);
    lv_port_indev_set_landscape(false);
    if (s_prev_scr != NULL) {
        lv_scr_load(s_prev_scr);
    }
}

static void typing_confirm(void)
{
    const char *text = lv_textarea_get_text(typing_ta);
    if (kb_mode == 1) {
        if (text != NULL && text[0] != '\0' && s_cbs.on_name_set != NULL) {
            s_cbs.on_name_set(text);
        }
    } else if (text != NULL && text[0] != '\0') {
        send_text_internal(text);
    }
    typing_close();
}

static void typing_kb_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_READY) {
        typing_confirm();
    } else if (code == LV_EVENT_CANCEL) {
        typing_close();
    }
}

static void typing_close_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    typing_close();
}

static void open_typing(int mode, const char *initial)
{
    kb_mode = mode;
    s_prev_scr = lv_scr_act();
    lv_port_disp_set_landscape(true);
    lv_port_indev_set_landscape(true);

    if (scr_typing == NULL) {
        build_typing_screen();
    }
    lv_textarea_set_max_length(typing_ta, (mode == 1) ? 23 : 48);
    lv_textarea_set_text(typing_ta, (initial != NULL) ? initial : "");
    lv_scr_load(scr_typing);
}

static void open_keyboard_msg_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    open_typing(0, "");
}

static void open_keyboard_name_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    open_typing(1, s_my_name);
}

static void build_typing_screen(void)
{
    scr_typing = lv_obj_create(NULL);
    lv_obj_set_style_bg_opa(scr_typing, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(scr_typing, lv_color_hex(COL_BG), 0);
    lv_obj_clear_flag(scr_typing, LV_OBJ_FLAG_SCROLLABLE);

    typing_ta = lv_textarea_create(scr_typing);
    lv_obj_set_pos(typing_ta, 8, 8);
    lv_obj_set_size(typing_ta, 252, 36);
    lv_textarea_set_one_line(typing_ta, true);
    lv_textarea_set_max_length(typing_ta, 48);
    lv_textarea_set_placeholder_text(typing_ta, "type...");
    lv_obj_set_style_text_font(typing_ta, ui_font, 0);
    lv_obj_set_style_text_color(typing_ta, lv_color_hex(COL_TEXT), 0);
    lv_obj_set_style_bg_color(typing_ta, lv_color_hex(0x121212), 0);
    lv_obj_set_style_border_color(typing_ta, lv_color_hex(COL_ACCENT), 0);

    lv_obj_t *close_btn = make_box(scr_typing, 272, 8, 40, 36, 6,
                                   lv_color_hex(0x121212), LV_OPA_COVER,
                                   lv_color_hex(COL_ACCENT), 1);
    lv_obj_add_flag(close_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *close_lab = make_label(close_btn, "X", lv_color_hex(COL_ACCENT), ui_font);
    lv_obj_align(close_lab, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_event_cb(close_btn, typing_close_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *kb = lv_keyboard_create(scr_typing);
    lv_obj_set_size(kb, 320, 190);
    lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_textarea(kb, typing_ta);
    lv_keyboard_set_mode(kb, LV_KEYBOARD_MODE_TEXT_LOWER);
    lv_obj_add_event_cb(kb, typing_kb_cb, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(kb, typing_kb_cb, LV_EVENT_CANCEL, NULL);
}

static lv_obj_t *make_chip(lv_obj_t *parent, lv_coord_t x, lv_coord_t y,
                           lv_coord_t w, lv_coord_t h, const char *text,
                           lv_event_cb_t cb, void *user_data)
{
    lv_obj_t *chip = make_box(parent, x, y, w, h, 6,
                              lv_color_hex(0x0D0D0D), LV_OPA_COVER,
                              lv_color_hex(COL_LINE), 1);
    lv_obj_add_flag(chip, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *lab = make_label(chip, text, lv_color_hex(COL_TEXT), ui_font);
    lv_obj_align(lab, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_event_cb(chip, cb, LV_EVENT_CLICKED, user_data);
    /* pressed state: neon-green fill so taps feel responsive */
    lv_obj_set_style_bg_color(chip, lv_color_hex(COL_ACCENT), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(chip, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_text_color(chip, lv_color_hex(0x000000), LV_STATE_PRESSED);
    return chip;
}

static void build_status_bar(lv_obj_t *parent)
{
    /* neon-green hairline on top */
    make_box(parent, 0, 0, SCR_W, 1, 0, lv_color_hex(COL_ACCENT), LV_OPA_COVER,
             lv_color_hex(COL_ACCENT), 0);

    lv_obj_t *bar = make_box(parent, 0, 1, SCR_W, STATUS_H - 1, 0,
                             lv_color_hex(COL_BAR), LV_OPA_COVER,
                             lv_color_hex(COL_LINE), 1);
    lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_BOTTOM, 0);

    /* settings entry */
    lv_obj_t *gear = make_box(bar, 2, 2, 30, 30, 6, lv_color_hex(0x000000),
                              LV_OPA_TRANSP, lv_color_hex(0x000000), 0);
    lv_obj_add_flag(gear, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *gear_lab = make_label(gear, LV_SYMBOL_SETTINGS, lv_color_hex(COL_MUTED), ui_font);
    lv_obj_align(gear_lab, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_event_cb(gear, settings_btn_cb, LV_EVENT_CLICKED, NULL);

    /* link dot + peer name */
    link_dot = make_box(bar, 40, 14, 8, 8, 4, lv_color_hex(COL_MUTED),
                        LV_OPA_COVER, lv_color_hex(COL_MUTED), 0);
    link_label = make_label(bar, "NO LINK", lv_color_hex(COL_MUTED), ui_font);
    lv_obj_set_pos(link_label, 52, 6);
    lv_obj_set_width(link_label, 110);
    lv_label_set_long_mode(link_label, LV_LABEL_LONG_DOT);

    /* battery */
    battery_icon = make_label(bar, LV_SYMBOL_BATTERY_3, lv_color_hex(COL_ACCENT), ui_font);
    lv_obj_set_pos(battery_icon, 182, 8);
    battery_label = make_label(bar, "85%", lv_color_hex(COL_TEXT), ui_font);
    lv_obj_align(battery_label, LV_ALIGN_RIGHT_MID, -6, 0);
}

static void build_chat_list(lv_obj_t *parent)
{
    chat_list = make_box(parent, 0, STATUS_H, SCR_W, CHAT_H, 0,
                         lv_color_hex(COL_BG), LV_OPA_COVER,
                         lv_color_hex(COL_BG), 0);
    lv_obj_set_style_pad_all(chat_list, 8, 0);
    lv_obj_set_style_pad_row(chat_list, 6, 0);
    lv_obj_set_flex_flow(chat_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(chat_list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    /* make_box() clears SCROLLABLE; chat must scroll to keep newest visible */
    lv_obj_add_flag(chat_list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_bottom(chat_list, 8, 0);
    lv_obj_set_scroll_dir(chat_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(chat_list, LV_SCROLLBAR_MODE_ON);
}

static void build_bottom_bar(lv_obj_t *parent)
{
    lv_obj_t *bar = make_box(parent, 0, SCR_H - BOTTOM_H, SCR_W, BOTTOM_H, 0,
                             lv_color_hex(COL_BAR), LV_OPA_COVER,
                             lv_color_hex(COL_LINE), 1);
    lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_TOP, 0);

    /* PTT: black circle + neon-green outline; pressed = green fill + black text */
    ptt_btn = make_box(bar, 8, 5, 60, 60, 30, lv_color_hex(0x0A0A0A), LV_OPA_TRANSP,
                       lv_color_hex(COL_ACCENT), 2);
    lv_obj_add_flag(ptt_btn, LV_OBJ_FLAG_CLICKABLE);
    ptt_label = make_label(ptt_btn, "HOLD\nTALK", lv_color_hex(COL_TEXT), ui_font);
    lv_obj_set_style_text_align(ptt_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(ptt_label, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_event_cb(ptt_btn, ptt_event_cb, LV_EVENT_ALL, NULL);

    /* 2x2 quick chips */
    make_chip(bar, 74, 2, 78, 30, "OK", chip_click_cb, (void *)"OK");
    make_chip(bar, 156, 2, 78, 30, "HI", chip_click_cb, (void *)"HI");
    make_chip(bar, 74, 34, 78, 30, "COMING", chip_click_cb, (void *)"COMING");
    make_chip(bar, 156, 34, 78, 30, LV_SYMBOL_KEYBOARD " TEXT",
              open_keyboard_msg_cb, NULL);
}

/* ---------------- settings screen (volume / brightness) ---------------- */
static void settings_refresh_labels(void)
{
    if (vol_label != NULL) {
        char buf[8];
        snprintf(buf, sizeof(buf), "%d%%", s_volume);
        lv_label_set_text(vol_label, buf);
    }
    if (bri_label != NULL) {
        char buf[8];
        snprintf(buf, sizeof(buf), "%d%%", s_brightness);
        lv_label_set_text(bri_label, buf);
    }
}

static void settings_set_volume(int v)
{
    if (v < 0) v = 0;
    if (v > 100) v = 100;
    s_volume = v;
    settings_refresh_labels();
    if (s_cbs.on_volume_set != NULL) {
        s_cbs.on_volume_set(v);
    }
}

static void settings_set_brightness(int v)
{
    if (v < 0) v = 0;
    if (v > 100) v = 100;
    s_brightness = v;
    settings_refresh_labels();
    if (s_cbs.on_brightness_set != NULL) {
        s_cbs.on_brightness_set(v);
    }
}

static void vol_minus_cb(lv_event_t *e) { LV_UNUSED(e); settings_set_volume(s_volume - 10); }
static void vol_plus_cb(lv_event_t *e) { LV_UNUSED(e); settings_set_volume(s_volume + 10); }
static void bri_minus_cb(lv_event_t *e) { LV_UNUSED(e); settings_set_brightness(s_brightness - 10); }
static void bri_plus_cb(lv_event_t *e) { LV_UNUSED(e); settings_set_brightness(s_brightness + 10); }

static void settings_back_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    lv_scr_load_anim(scr_main, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 180, 0, false);
}

static void settings_nodes_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    lv_scr_load_anim(scr_pair, LV_SCR_LOAD_ANIM_MOVE_LEFT, 180, 0, false);
}

static void make_adj_row(lv_obj_t *parent, lv_coord_t y, const char *title,
                         lv_obj_t **value_label,
                         lv_event_cb_t minus_cb, lv_event_cb_t plus_cb)
{
    lv_obj_t *row = make_box(parent, 8, y, 224, 46, 6,
                             lv_color_hex(0x121212), LV_OPA_COVER,
                             lv_color_hex(COL_LINE), 1);
    lv_obj_t *tag = make_label(row, title, lv_color_hex(COL_MUTED), ui_font);
    lv_obj_set_pos(tag, 10, 4);

    *value_label = make_label(row, "0%", lv_color_hex(COL_TEXT), ui_font);
    lv_obj_set_pos(*value_label, 10, 24);

    lv_obj_t *minus = make_box(row, 140, 8, 32, 30, 6, lv_color_hex(0x000000),
                               LV_OPA_TRANSP, lv_color_hex(COL_ACCENT), 1);
    lv_obj_add_flag(minus, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *ml = make_label(minus, "-", lv_color_hex(COL_ACCENT), ui_font);
    lv_obj_align(ml, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_event_cb(minus, minus_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *plus = make_box(row, 180, 8, 32, 30, 6, lv_color_hex(0x000000),
                              LV_OPA_TRANSP, lv_color_hex(COL_ACCENT), 1);
    lv_obj_add_flag(plus, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *pl = make_label(plus, "+", lv_color_hex(COL_ACCENT), ui_font);
    lv_obj_align(pl, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_event_cb(plus, plus_cb, LV_EVENT_CLICKED, NULL);
}

static void build_settings_screen(void)
{
    scr_settings = lv_obj_create(NULL);
    lv_obj_set_style_bg_opa(scr_settings, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(scr_settings, lv_color_hex(COL_BG), 0);
    lv_obj_clear_flag(scr_settings, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *bar = make_box(scr_settings, 0, 0, SCR_W, 34, 0,
                             lv_color_hex(COL_BAR), LV_OPA_COVER,
                             lv_color_hex(COL_LINE), 1);
    lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_BOTTOM, 0);

    lv_obj_t *back = make_box(bar, 2, 2, 30, 30, 6, lv_color_hex(0x000000),
                              LV_OPA_TRANSP, lv_color_hex(0x000000), 0);
    lv_obj_add_flag(back, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *back_lab = make_label(back, LV_SYMBOL_LEFT, lv_color_hex(COL_TEXT), ui_font);
    lv_obj_align(back_lab, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_event_cb(back, settings_back_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *title = make_label(bar, "SETTINGS", lv_color_hex(COL_TEXT), ui_font);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, 0);

    make_adj_row(scr_settings, 44, "VOLUME", &vol_label,
                 vol_minus_cb, vol_plus_cb);
    make_adj_row(scr_settings, 98, "BRIGHTNESS", &bri_label,
                 bri_minus_cb, bri_plus_cb);

    lv_obj_t *nodes_row = make_box(scr_settings, 8, 152, 224, 46, 6,
                                   lv_color_hex(0x121212), LV_OPA_COVER,
                                   lv_color_hex(COL_LINE), 1);
    lv_obj_add_flag(nodes_row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *nl = make_label(nodes_row, "NODES", lv_color_hex(COL_TEXT), ui_font);
    lv_obj_set_pos(nl, 10, 14);
    lv_obj_t *arrow = make_label(nodes_row, LV_SYMBOL_RIGHT, lv_color_hex(COL_ACCENT), ui_font);
    lv_obj_align(arrow, LV_ALIGN_RIGHT_MID, -10, 0);
    lv_obj_add_event_cb(nodes_row, settings_nodes_cb, LV_EVENT_CLICKED, NULL);

    settings_refresh_labels();
}

static void build_main_screen(void)
{
    scr_main = lv_scr_act();
    lv_obj_set_style_bg_opa(scr_main, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(scr_main, lv_color_hex(COL_BG), 0);
    lv_obj_clear_flag(scr_main, LV_OBJ_FLAG_SCROLLABLE);

    build_status_bar(scr_main);
    build_chat_list(scr_main);
    build_bottom_bar(scr_main);
}

/* ---------------- pairing screen ---------------- */
static void pair_back_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    lv_scr_load_anim(scr_main, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 180, 0, false);
}

static void scan_btn_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    if (s_cbs.on_scan_request) {
        s_cbs.on_scan_request();
    }
    ui_app_add_system_message("SCANNING...");
}

static void make_peer_row(lv_obj_t *parent, const char *name, const char *rssi_text)
{
    lv_obj_t *row = make_box(parent, 0, 0, 224, 44, 6,
                             lv_color_hex(0x121212), LV_OPA_COVER,
                             lv_color_hex(COL_LINE), 1);
    lv_obj_set_width(row, lv_pct(100));

    lv_obj_t *name_lab = make_label(row, name, lv_color_hex(COL_TEXT), ui_font);
    lv_obj_set_pos(name_lab, 10, 12);

    lv_obj_t *rssi_lab = make_label(row, rssi_text, lv_color_hex(COL_MUTED), ui_font);
    lv_obj_set_pos(rssi_lab, 130, 12);
}

static void build_pair_screen(void)
{
    scr_pair = lv_obj_create(NULL);
    lv_obj_set_style_bg_opa(scr_pair, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(scr_pair, lv_color_hex(COL_BG), 0);
    lv_obj_clear_flag(scr_pair, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *bar = make_box(scr_pair, 0, 0, SCR_W, 32, 0,
                             lv_color_hex(COL_BAR), LV_OPA_COVER,
                             lv_color_hex(COL_LINE), 1);
    lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_BOTTOM, 0);

    lv_obj_t *back = make_box(bar, 2, 2, 28, 28, 4, lv_color_hex(0x000000),
                              LV_OPA_TRANSP, lv_color_hex(0x000000), 0);
    lv_obj_add_flag(back, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *back_lab = make_label(back, LV_SYMBOL_LEFT, lv_color_hex(COL_TEXT), ui_font);
    lv_obj_align(back_lab, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_event_cb(back, pair_back_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *title = make_label(bar, "NODES", lv_color_hex(COL_TEXT), ui_font);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, 0);

    lv_obj_t *scan = make_box(bar, 210, 2, 28, 28, 4, lv_color_hex(0x000000),
                              LV_OPA_TRANSP, lv_color_hex(0x000000), 0);
    lv_obj_add_flag(scan, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *scan_lab = make_label(scan, LV_SYMBOL_REFRESH, lv_color_hex(COL_MUTED), ui_font);
    lv_obj_align(scan_lab, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_event_cb(scan, scan_btn_cb, LV_EVENT_CLICKED, NULL);

    /* local node card */
    lv_obj_t *own = make_box(scr_pair, 8, 38, 224, 46, 6,
                             lv_color_hex(0x121212), LV_OPA_COVER,
                             lv_color_hex(COL_LINE), 1);
    own_name_label = make_label(own, s_my_name, lv_color_hex(COL_TEXT), ui_font);
    lv_obj_set_pos(own_name_label, 10, 4);
    lv_obj_t *own_info = make_label(own, "CH 6 | NO LINK", lv_color_hex(COL_MUTED), ui_font);
    lv_obj_set_pos(own_info, 10, 24);

    /* editable local node name */
    lv_obj_t *name_row = make_box(scr_pair, 8, 88, 224, 44, 6,
                                  lv_color_hex(0x121212), LV_OPA_COVER,
                                  lv_color_hex(COL_LINE), 1);
    lv_obj_t *name_tag = make_label(name_row, "MY NAME", lv_color_hex(COL_MUTED), ui_font);
    lv_obj_set_pos(name_tag, 10, 4);
    settings_name_label = make_label(name_row, s_my_name, lv_color_hex(COL_TEXT), ui_font);
    lv_obj_set_pos(settings_name_label, 10, 22);
    lv_obj_t *edit_btn = make_box(name_row, 160, 8, 54, 28, 6,
                                  lv_color_hex(0x000000), LV_OPA_TRANSP,
                                  lv_color_hex(COL_ACCENT), 1);
    lv_obj_add_flag(edit_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *edit_lab = make_label(edit_btn, "EDIT", lv_color_hex(COL_ACCENT), ui_font);
    lv_obj_align(edit_lab, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_event_cb(edit_btn, open_keyboard_name_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *section = make_label(scr_pair, "NEARBY", lv_color_hex(COL_MUTED), ui_font);
    lv_obj_set_pos(section, 10, 138);

    pair_peer_list = make_box(scr_pair, 8, 156, 224, 150, 0,
                              lv_color_hex(0x000000), LV_OPA_TRANSP,
                              lv_color_hex(0x000000), 0);
    lv_obj_add_flag(pair_peer_list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(pair_peer_list, LV_DIR_VER);
    lv_obj_set_style_pad_row(pair_peer_list, 4, 0);
    lv_obj_set_flex_flow(pair_peer_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scrollbar_mode(pair_peer_list, LV_SCROLLBAR_MODE_OFF);

    ui_app_set_peers(NULL, 0);
}

/* ---------------- public API ---------------- */
void ui_app_init(void)
{
    ui_font = &lv_font_montserrat_16;

    memset(&s_cbs, 0, sizeof(s_cbs));

    build_main_screen();
    build_pair_screen();
    build_settings_screen();
    ui_app_set_name(s_my_name);

    ui_app_set_battery(s_battery);
    ui_app_set_link(UI_LINK_DISCONNECTED, NULL);

    ui_app_add_system_message("CH 6 | READY");

    ESP_LOGI(TAG, "UI ready (240x320 portrait, black/neon-green theme)");
}

void ui_app_set_callbacks(const ui_callbacks_t *cbs)
{
    if (cbs) {
        s_cbs = *cbs;
    } else {
        memset(&s_cbs, 0, sizeof(s_cbs));
    }
}

void ui_app_set_link(ui_link_state_t state, const char *peer_name)
{
    s_link = state;
    link_update_visual();

    if (peer_name && peer_name[0] != '\0') {
        lv_label_set_text(link_label, peer_name);
    } else {
        lv_label_set_text(link_label,
                          state == UI_LINK_SEARCHING ? "SEARCHING" : "NO LINK");
    }
}

void ui_app_set_battery(uint8_t percent)
{
    if (percent > 100) {
        percent = 100;
    }
    s_battery = percent;
    if (!battery_icon) {
        return;
    }

    const char *symbol;
    lv_color_t color;
    if (percent >= 80) {
        symbol = LV_SYMBOL_BATTERY_FULL;
        color = lv_color_hex(COL_ACCENT);
    } else if (percent >= 55) {
        symbol = LV_SYMBOL_BATTERY_3;
        color = lv_color_hex(COL_ACCENT);
    } else if (percent >= 30) {
        symbol = LV_SYMBOL_BATTERY_2;
        color = lv_color_hex(COL_WARN);
    } else if (percent >= 10) {
        symbol = LV_SYMBOL_BATTERY_1;
        color = lv_color_hex(COL_WARN);
    } else {
        symbol = LV_SYMBOL_BATTERY_EMPTY;
        color = lv_color_hex(COL_DANGER);
    }
    lv_label_set_text(battery_icon, symbol);
    lv_obj_set_style_text_color(battery_icon, color, 0);

    char buf[8];
    snprintf(buf, sizeof(buf), "%u%%", (unsigned)percent);
    lv_label_set_text(battery_label, buf);
}

static void trim_oldest_message(void)
{
    while (s_msg_count >= MAX_MESSAGES) {
        lv_obj_t *first = lv_obj_get_child(chat_list, 0);
        if (!first) {
            s_msg_count = 0;
            break;
        }
        lv_obj_del(first);
        s_msg_count--;
    }
}

void ui_app_add_message(const char *text, bool outgoing)
{
    if (!text || text[0] == '\0' || !chat_list) {
        return;
    }
    trim_oldest_message();

    lv_obj_t *row = lv_obj_create(chat_list);
    style_reset(row);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row,
                          outgoing ? LV_FLEX_ALIGN_END : LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *bubble = lv_obj_create(row);
    style_reset(bubble);
    lv_obj_set_height(bubble, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(bubble, 8, 0);
    lv_obj_set_style_bg_opa(bubble, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(bubble,
                              lv_color_hex(outgoing ? COL_OUT_BUBBLE : COL_IN_BUBBLE), 0);
    lv_obj_set_style_border_width(bubble, outgoing ? 0 : 1, 0);
    lv_obj_set_style_border_color(bubble, lv_color_hex(COL_IN_BORDER), 0);
    lv_obj_set_style_pad_hor(bubble, 9, 0);
    lv_obj_set_style_pad_ver(bubble, 6, 0);
    lv_obj_set_style_max_width(bubble, BUBBLE_MAX_W, 0);
    lv_obj_clear_flag(bubble, LV_OBJ_FLAG_SCROLLABLE);

    lv_point_t txt_size;
    lv_txt_get_size(&txt_size, text, ui_font, 0, 0, LABEL_MAX_W, LV_TEXT_FLAG_NONE);

    lv_obj_t *label = make_label(bubble, text,
                                 lv_color_hex(outgoing ? 0x000000 : COL_TEXT), ui_font);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(label, txt_size.x > 0 ? txt_size.x : 1);

    s_msg_count++;
    lv_obj_update_layout(chat_list);
    lv_obj_scroll_to_y(chat_list, LV_COORD_MAX, LV_ANIM_OFF);
}

void ui_app_add_system_message(const char *text)
{
    if (!text || text[0] == '\0' || !chat_list) {
        return;
    }
    trim_oldest_message();

    lv_obj_t *row = lv_obj_create(chat_list);
    style_reset(row);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *label = make_label(row, text, lv_color_hex(COL_MUTED), ui_font);
    lv_obj_set_width(label, lv_pct(100));
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);

    s_msg_count++;
    lv_obj_update_layout(chat_list);
    lv_obj_scroll_to_y(chat_list, LV_COORD_MAX, LV_ANIM_OFF);
}

void ui_app_set_voice_level(uint8_t level)
{
    s_voice_level = level > 100 ? 100 : level;
    (void)s_voice_level;
}

/* ---------------- flat-mesh peer list ---------------- */
void ui_app_set_peers(const ui_peer_t *peers, int count)
{
    if (count < 0) {
        count = 0;
    }
    s_peer_count = count;

    if (count == 0) {
        lv_label_set_text(link_label, "NO LINK");
    } else if (count == 1 && peers != NULL && peers[0].name[0] != '\0') {
        lv_label_set_text(link_label, peers[0].name);
    } else {
        char buf[24];
        snprintf(buf, sizeof(buf), "%d NODES", count);
        lv_label_set_text(link_label, buf);
    }
    s_link = (count > 0) ? UI_LINK_CONNECTED : UI_LINK_DISCONNECTED;
    link_update_visual();

    if (pair_peer_list == NULL) {
        return;
    }
    lv_obj_clean(pair_peer_list);
    if (count == 0 || peers == NULL) {
        lv_obj_t *empty = make_label(pair_peer_list, "NO NODES FOUND",
                                     lv_color_hex(COL_MUTED), ui_font);
        lv_obj_set_pos(empty, 10, 12);
        return;
    }
    for (int i = 0; i < count; i++) {
        if (peers[i].name[0] == '\0') {
            continue;
        }
        char rssi_buf[24];
        snprintf(rssi_buf, sizeof(rssi_buf), "%d dBm", (int)peers[i].rssi);
        make_peer_row(pair_peer_list, peers[i].name, rssi_buf);
    }
}


void ui_app_set_name(const char *name)
{
    if (name == NULL || name[0] == '\0') {
        return;
    }
    strncpy(s_my_name, name, sizeof(s_my_name) - 1);
    s_my_name[sizeof(s_my_name) - 1] = '\0';
    if (settings_name_label != NULL) {
        lv_label_set_text(settings_name_label, s_my_name);
    }
    if (own_name_label != NULL) {
        lv_label_set_text(own_name_label, s_my_name);
    }
}

void ui_app_add_message_from(const char *sender, const char *text)
{
    if (text == NULL || text[0] == '\0') {
        return;
    }
    if (sender != NULL && sender[0] != '\0') {
        char buf[128];
        snprintf(buf, sizeof(buf), "%s: %s", sender, text);
        ui_app_add_message(buf, false);
    } else {
        ui_app_add_message(text, false);
    }
}
