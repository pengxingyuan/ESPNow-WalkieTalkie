/**
 * @file net_link.c
 * @brief ESP-NOW link layer: broadcast discovery + text messages
 *
 * Frame format (little endian):
 *   magic(2) ver(1) type(1) flags(1) seq(2) src(6) dst(6) len(2) payload(len) crc16(2)
 *   flags: bit0/1 = voice start/end, bit2-5 = TTL (remaining relay hops), bit6-7 reserved
 *   header 21 bytes; ESP-NOW v1 max payload 250 bytes => payload <= 227 bytes.
 */
#include "net_link.h"

#include <stdio.h>
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_now.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "app_ui.h"
#include "voice.h"

static const char *TAG = "NET_LINK";

#define ESPNOW_CHANNEL      6
#define FRAME_MAGIC         0xE5A5
#define FRAME_VER           1
#define NL_NAME_MAX         24
#define MAX_PAYLOAD         200
#define RX_QUEUE_LEN        8
#define HEARTBEAT_MS        3000

/* relay (multi-hop mesh) */
#define FLAG_VOICE_START    0x01
#define FLAG_VOICE_END      0x02
#define FLAG_TTL_SHIFT      2
#define FLAG_TTL_MASK       0x3C
#define RELAY_MAX_HOPS      3
#define RELAY_QUEUE_LEN     8
#define RELAY_RATE_PER_SEC  20
#define RELAY_JITTER_MIN_MS 5
#define RELAY_JITTER_MAX_MS 25
#define DEDUP_SIZE          32
#define DEDUP_WINDOW_MS     30000

/* message types */
#define MSG_HELLO           1
#define MSG_TEXT            2
#define MSG_PTT             3
#define MSG_VOICE           4

/* ---------------- frame header (packed) ---------------- */
typedef struct __attribute__((packed)) {
    uint16_t magic;
    uint8_t ver;
    uint8_t type;
    uint8_t flags;
    uint16_t seq;
    uint8_t src[6];
    uint8_t dst[6];
    uint16_t len;
} frame_hdr_t;

/* ---------------- RX queue item ---------------- */
typedef struct {
    uint8_t type;
    uint8_t flags;
    uint16_t seq;
    uint16_t len;
    uint8_t mac[6];
    int8_t rssi;
    uint16_t raw_len;
    uint8_t raw[sizeof(frame_hdr_t) + MAX_PAYLOAD + 2];
    char data[MAX_PAYLOAD + 1];
} rx_item_t;

#define MAX_PEERS 8
#define PEER_TIMEOUT_MS 10000

typedef struct {
    uint8_t mac[6];
    char name[NL_NAME_MAX];
    int8_t rssi;
    int64_t last_ms;
} peer_slot_t;

static peer_slot_t s_peers[MAX_PEERS];
static int s_peer_num;
static ui_peer_t s_last_ui_peers[MAX_PEERS];
static int s_last_ui_count;

static const uint8_t BCAST_MAC[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

static uint8_t s_mac[6];
static char s_name[NL_NAME_MAX];
static uint16_t s_seq;
static QueueHandle_t s_rx_queue;
static esp_timer_handle_t s_hb_timer;

/* ---------------- helpers ---------------- */
static uint16_t crc16_ccitt(const uint8_t *data, size_t len)
{
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (int b = 0; b < 8; b++) {
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

static bool same_mac(const uint8_t *a, const uint8_t *b)
{
    return memcmp(a, b, 6) == 0;
}

/* ---------------- dedup (src MAC + seq) ---------------- */
typedef struct {
    uint8_t mac[6];
    uint16_t seq;
    int64_t seen_ms;
    bool used;
} dedup_entry_t;

static dedup_entry_t s_dedup[DEDUP_SIZE];
static int s_dedup_next;

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

/* returns true when this (src,seq) was already seen inside the window */
static bool dedup_seen(const uint8_t *mac, uint16_t seq)
{
    int64_t now = now_ms();
    for (int i = 0; i < DEDUP_SIZE; i++) {
        if (!s_dedup[i].used || s_dedup[i].seq != seq) {
            continue;
        }
        if (same_mac(s_dedup[i].mac, mac) && (now - s_dedup[i].seen_ms) < DEDUP_WINDOW_MS) {
            return true;
        }
    }
    dedup_entry_t *e = &s_dedup[s_dedup_next];
    s_dedup_next = (s_dedup_next + 1) % DEDUP_SIZE;
    memcpy(e->mac, mac, 6);
    e->seq = seq;
    e->seen_ms = now;
    e->used = true;
    return false;
}

/* ---------------- relay queue ---------------- */
typedef struct {
    uint8_t buf[sizeof(frame_hdr_t) + MAX_PAYLOAD + 2];
    uint16_t len;
    int64_t due_ms;
} relay_item_t;

static relay_item_t s_relay_q[RELAY_QUEUE_LEN];
static int s_relay_num;
static int64_t s_relay_win_ms;
static int s_relay_win_count;

static void relay_maybe(const rx_item_t *item, uint8_t ttl)
{
    if (ttl == 0 || item->raw_len < sizeof(frame_hdr_t) + 2) {
        return;
    }

    int64_t now = now_ms();
    if (now - s_relay_win_ms >= 1000) {
        s_relay_win_ms = now;
        s_relay_win_count = 0;
    }
    if (s_relay_win_count >= RELAY_RATE_PER_SEC) {
        ESP_LOGW(TAG, "relay rate limit hit, dropping");
        return;
    }
    if (s_relay_num >= RELAY_QUEUE_LEN) {
        ESP_LOGW(TAG, "relay queue full, dropping");
        return;
    }

    relay_item_t *r = &s_relay_q[s_relay_num++];
    memcpy(r->buf, item->raw, item->raw_len);
    r->len = item->raw_len;

    frame_hdr_t *h = (frame_hdr_t *)r->buf;
    h->flags = (uint8_t)((h->flags & ~FLAG_TTL_MASK) | ((ttl - 1) << FLAG_TTL_SHIFT));

    uint16_t crc = crc16_ccitt(r->buf, r->len - 2);
    r->buf[r->len - 2] = (uint8_t)(crc & 0xFF);
    r->buf[r->len - 1] = (uint8_t)(crc >> 8);

    int jitter = RELAY_JITTER_MIN_MS + (int)(esp_random() % (RELAY_JITTER_MAX_MS - RELAY_JITTER_MIN_MS + 1));
    r->due_ms = now + jitter;

    ESP_LOGI(TAG, "relay tx type=%u seq=%u ttl=%u", h->type, h->seq, (unsigned)(ttl - 1));
}

static void relay_process(void)
{
    int64_t now = now_ms();
    for (int i = 0; i < s_relay_num; ) {
        if (now < s_relay_q[i].due_ms) {
            i++;
            continue;
        }
        esp_now_send(BCAST_MAC, s_relay_q[i].buf, s_relay_q[i].len);
        s_relay_win_count++;
        s_relay_q[i] = s_relay_q[--s_relay_num]; /* move last item into this slot */
    }
}

/* ---------------- TX ---------------- */
static esp_err_t send_frame(uint8_t type, const void *payload, uint16_t len)
{
    if (len > MAX_PAYLOAD) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t buf[sizeof(frame_hdr_t) + MAX_PAYLOAD + 2];
    frame_hdr_t *hdr = (frame_hdr_t *)buf;

    hdr->magic = FRAME_MAGIC;
    hdr->ver = FRAME_VER;
    hdr->type = type;
    hdr->flags = 0;
    if (type == MSG_HELLO || type == MSG_TEXT) {
        hdr->flags = (uint8_t)(RELAY_MAX_HOPS << FLAG_TTL_SHIFT);
    }
    hdr->seq = ++s_seq;
    memcpy(hdr->src, s_mac, 6);
    memcpy(hdr->dst, BCAST_MAC, 6);
    hdr->len = len;
    if (len > 0 && payload != NULL) {
        memcpy(buf + sizeof(frame_hdr_t), payload, len);
    }

    size_t total = sizeof(frame_hdr_t) + len;
    uint16_t crc = crc16_ccitt(buf, total);
    buf[total++] = (uint8_t)(crc & 0xFF);
    buf[total++] = (uint8_t)(crc >> 8);

    esp_err_t err = esp_now_send(BCAST_MAC, buf, total);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_now_send failed: %s", esp_err_to_name(err));
    }
    return err;
}

static void send_hello(void)
{
    send_frame(MSG_HELLO, s_name, (uint16_t)strlen(s_name) + 1);
}

void net_link_send_hello(void)
{
    if (s_name[0] != '\0') {
        send_hello();
    }
}

void net_link_send_text(const char *text)
{
    if (text == NULL || text[0] == '\0') {
        return;
    }
    send_frame(MSG_TEXT, text, (uint16_t)strlen(text) + 1);
}

esp_err_t net_link_send_voice(const uint8_t *payload, uint16_t len)
{
    return send_frame(MSG_VOICE, payload, len);
}

/* ---------------- RX ---------------- */
static void on_recv(const esp_now_recv_info_t *info, const uint8_t *data, int len)
{
    if (info == NULL || data == NULL || len < (int)(sizeof(frame_hdr_t) + 2)) {
        return;
    }
    const frame_hdr_t *hdr = (const frame_hdr_t *)data;
    if (hdr->magic != FRAME_MAGIC || hdr->ver != FRAME_VER) {
        uint16_t magic = 0;
        memcpy(&magic, data, 2);
        ESP_LOGW(TAG, "rx unknown frame len=%d magic=0x%04X", len, magic);
        return;
    }
    if (hdr->type != MSG_VOICE) {
        ESP_LOGI(TAG, "rx frame type=%u len=%d ttl=%u from %02X:%02X:%02X:%02X:%02X:%02X",
                 hdr->type, len, (unsigned)((hdr->flags & FLAG_TTL_MASK) >> FLAG_TTL_SHIFT),
                 hdr->src[0], hdr->src[1], hdr->src[2],
                 hdr->src[3], hdr->src[4], hdr->src[5]);
    }
    if (same_mac(hdr->src, s_mac)) {
        return; /* our own broadcast */
    }

    size_t hdr_len = sizeof(frame_hdr_t);
    int payload_len = len - (int)hdr_len - 2;
    if (payload_len < 0 || (uint16_t)payload_len != hdr->len) {
        return;
    }

    uint16_t crc_rx = (uint16_t)(data[len - 2] | (data[len - 1] << 8));
    if (crc16_ccitt(data, len - 2) != crc_rx) {
        ESP_LOGW(TAG, "crc mismatch");
        return;
    }

    rx_item_t item = {0};
    item.type = hdr->type;
    item.flags = hdr->flags;
    item.seq = hdr->seq;
    item.len = (uint16_t)payload_len;
    item.raw_len = (uint16_t)len;
    if (len <= (int)sizeof(item.raw)) {
        memcpy(item.raw, data, (size_t)len);
    } else {
        item.raw_len = 0;
    }
    if (payload_len > 0) {
        size_t n = (size_t)payload_len;
        if (n > MAX_PAYLOAD) {
            n = MAX_PAYLOAD;
        }
        memcpy(item.data, data + hdr_len, n);
        item.data[n] = '\0';
        memcpy(item.mac, hdr->src, 6);
        item.rssi = info->rx_ctrl ? (int8_t)info->rx_ctrl->rssi : 0;
    }
    ESP_LOGD(TAG, "rx type=%u len=%u from %02X:%02X",
             item.type, item.len, hdr->src[4], hdr->src[5]);

    if (s_rx_queue != NULL) {
        xQueueSend(s_rx_queue, &item, 0); /* drop when full, never block WiFi task */
    }
}

static void on_send(const esp_now_send_info_t *info, esp_now_send_status_t status)
{
    if (status != ESP_NOW_SEND_SUCCESS) {
        ESP_LOGW(TAG, "send status: fail");
    }
}

/* ---------------- drain queue (LVGL thread) ---------------- */
/* ---------------- flat-mesh peer table ---------------- */
static peer_slot_t *peer_find(const uint8_t *mac)
{
    for (int i = 0; i < s_peer_num; i++) {
        if (same_mac(s_peers[i].mac, mac)) {
            return &s_peers[i];
        }
    }
    return NULL;
}

static void peer_update(const uint8_t *mac, const char *name, int8_t rssi)
{
    peer_slot_t *p = peer_find(mac);
    if (p == NULL) {
        if (s_peer_num >= MAX_PEERS) {
            return;
        }
        p = &s_peers[s_peer_num++];
        memcpy(p->mac, mac, 6);
        p->name[0] = '\0';
    }
    if (name != NULL && name[0] != '\0') {
        strncpy(p->name, name, sizeof(p->name) - 1);
        p->name[sizeof(p->name) - 1] = '\0';
    }
    p->rssi = rssi;
    p->last_ms = esp_timer_get_time() / 1000;
}

static void peer_prune(void)
{
    int64_t now = esp_timer_get_time() / 1000;
    for (int i = 0; i < s_peer_num; ) {
        if (now - s_peers[i].last_ms > PEER_TIMEOUT_MS) {
            s_peers[i] = s_peers[--s_peer_num];
        } else {
            i++;
        }
    }
}

static void sync_peers_ui(void)
{
    peer_prune();

    ui_peer_t ui[MAX_PEERS];
    int n = 0;
    for (int i = 0; i < s_peer_num; i++) {
        if (s_peers[i].name[0] == '\0') {
            continue;
        }
        strncpy(ui[n].name, s_peers[i].name, sizeof(ui[n].name) - 1);
        ui[n].name[sizeof(ui[n].name) - 1] = '\0';
        ui[n].rssi = s_peers[i].rssi;
        n++;
    }

    if (n != s_last_ui_count) {
        char logbuf[128];
        int off = 0;
        for (int i = 0; i < n && off < (int)sizeof(logbuf) - 1; i++) {
            off += snprintf(logbuf + off, sizeof(logbuf) - off, "%s ", ui[i].name);
        }
        logbuf[sizeof(logbuf) - 1] = '\0';
        ESP_LOGI(TAG, "online peers (%d): %s", n, logbuf);
    }

    bool changed = (n != s_last_ui_count);
    for (int i = 0; !changed && i < n; i++) {
        if (strcmp(ui[i].name, s_last_ui_peers[i].name) != 0) {
            changed = true;
        }
    }
    if (!changed) {
        return;
    }

    memcpy(s_last_ui_peers, ui, sizeof(ui_peer_t) * n);
    s_last_ui_count = n;
    ui_app_set_peers(ui, n);
}


static const char *peer_name_of(const uint8_t *mac)
{
    peer_slot_t *p = peer_find(mac);
    return (p != NULL && p->name[0] != '\0') ? p->name : "?";
}

static void process_rx_item(rx_item_t *item)
{
    uint8_t ttl = (uint8_t)((item->flags & FLAG_TTL_MASK) >> FLAG_TTL_SHIFT);
    bool relayable = (item->type == MSG_HELLO || item->type == MSG_TEXT);

    /* voice is real-time: never dedup or relay it */
    if (item->type != MSG_VOICE) {
        if (dedup_seen(item->mac, item->seq)) {
            ESP_LOGI(TAG, "dedup drop type=%u seq=%u from %02X:%02X",
                     item->type, item->seq, item->mac[4], item->mac[5]);
            return;
        }
    }

    if (ttl < RELAY_MAX_HOPS && item->type != MSG_VOICE) {
        ESP_LOGI(TAG, "rx relayed type=%u seq=%u hop=%u",
                 item->type, item->seq, (unsigned)(RELAY_MAX_HOPS - ttl));
    }

    switch (item->type) {
    case MSG_HELLO:
        peer_update(item->mac, item->data, item->rssi);
        break;

    case MSG_TEXT: {
        peer_update(item->mac, NULL, item->rssi);
        char text[MAX_PAYLOAD + 1];
        strncpy(text, item->data, sizeof(text) - 1);
        text[sizeof(text) - 1] = '\0';
        ui_app_add_message_from(peer_name_of(item->mac), text);
        break;
    }

    case MSG_PTT:
        ESP_LOGI(TAG, "PTT event from peer: %s", item->data);
        break;

    case MSG_VOICE:
        voice_rx_push((const uint8_t *)item->data, item->len);
        break;

    default:
        break;
    }

    if (relayable && ttl > 0) {
        relay_maybe(item, ttl);
    }
}

void net_link_process(void)
{
    if (s_rx_queue == NULL) {
        return;
    }

    relay_process();

    rx_item_t item;
    while (xQueueReceive(s_rx_queue, &item, 0) == pdTRUE) {
        process_rx_item(&item);
    }
    sync_peers_ui();
}

/* ---------------- heartbeat ---------------- */
static void heartbeat_cb(void *arg)
{
    (void)arg;
    send_hello();
}

/* ---------------- init ---------------- */
static void name_load_from_nvs(void)
{
    nvs_handle_t h;
    s_name[0] = '\0';
    if (nvs_open("nodecfg", NVS_READONLY, &h) == ESP_OK) {
        size_t len = sizeof(s_name);
        if (nvs_get_str(h, "name", s_name, &len) != ESP_OK) {
            s_name[0] = '\0';
        }
        nvs_close(h);
    }
}

static void name_save_to_nvs(void)
{
    nvs_handle_t h;
    if (nvs_open("nodecfg", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_str(h, "name", s_name);
        nvs_commit(h);
        nvs_close(h);
    }
}

static void init_nvs(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
}

static void init_wifi(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE));
    /* ESP-NOW is latency sensitive: disable power save */
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

    esp_wifi_get_mac(WIFI_IF_STA, s_mac);
}

static void init_espnow(void)
{
    ESP_ERROR_CHECK(esp_now_init());
    ESP_ERROR_CHECK(esp_now_register_recv_cb(on_recv));
    ESP_ERROR_CHECK(esp_now_register_send_cb(on_send));

    esp_now_peer_info_t peer = {0};
    memcpy(peer.peer_addr, BCAST_MAC, 6);
    peer.channel = 0; /* current channel */
    peer.ifidx = WIFI_IF_STA;
    peer.encrypt = false;
    ESP_ERROR_CHECK(esp_now_add_peer(&peer));
}

void net_link_init(void)
{
    init_nvs();
    init_wifi();

    name_load_from_nvs();
    if (s_name[0] == '\0') {
        snprintf(s_name, sizeof(s_name), "NODE-%02X%02X", s_mac[4], s_mac[5]);
    }

    init_espnow();
    ESP_LOGI(TAG, "local node: %s, channel %d", s_name, ESPNOW_CHANNEL);

    s_rx_queue = xQueueCreate(RX_QUEUE_LEN, sizeof(rx_item_t));
    configASSERT(s_rx_queue != NULL);

    const esp_timer_create_args_t hb_args = {
        .callback = &heartbeat_cb,
        .name = "espnow_hb",
    };
    ESP_ERROR_CHECK(esp_timer_create(&hb_args, &s_hb_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(s_hb_timer, HEARTBEAT_MS * 1000));

    send_hello();
}

const char *net_link_get_name(void)
{
    return s_name;
}


void net_link_set_name(const char *name)
{
    if (name == NULL || name[0] == '\0') {
        return;
    }
    strncpy(s_name, name, sizeof(s_name) - 1);
    s_name[sizeof(s_name) - 1] = '\0';
    name_save_to_nvs();
    send_hello();
    ESP_LOGI(TAG, "node name -> %s", s_name);
}
