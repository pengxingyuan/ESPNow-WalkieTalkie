/**
 * @file voice.c
 * @brief Push-to-talk voice over ESP-NOW.
 *
 * TX: I2S mic 16 kHz -> mono -> IMA ADPCM (4 bit) -> 20 ms frames -> ESP-NOW broadcast.
 *     Packet payload: seq(2 LE) + flags(1) + adpcm bytes (160 for 20 ms).
 *     flags: bit0 = start of talk burst, bit1 = end of talk burst.
 * RX: packets -> queue -> decode -> speaker. Half duplex: no playback while TX.
 */
#include "voice.h"

#include <stdbool.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "adpcm.h"
#include "audio.h"
#include "net_link.h"

static const char *TAG = "VOICE";

#define FRAME_SAMPLES   320 /* 20 ms @ 16 kHz */
#define FRAME_BYTES     (FRAME_SAMPLES / 2)
#define VOICE_FLAG_START 0x01
#define VOICE_FLAG_END   0x02
#define MAX_PAYLOAD     200
#define RX_QUEUE_LEN    16

typedef struct {
    uint16_t len;
    uint8_t data[MAX_PAYLOAD];
} voice_pkt_t;

static QueueHandle_t s_rx_q;
static TaskHandle_t s_tx_task;
static TaskHandle_t s_rx_task;
static bool s_inited;

static volatile bool s_tx_active;
static volatile bool s_tx_stop_req;
static volatile bool s_tx_start_pending;
static volatile bool s_rx_active;

static uint16_t s_tx_seq;
static adpcm_state_t s_enc;
static adpcm_state_t s_dec;

static int16_t s_pcm[FRAME_SAMPLES];
static int16_t s_pcm_out[FRAME_SAMPLES];
static uint8_t s_enc_buf[FRAME_BYTES];
static uint8_t s_payload[3 + FRAME_BYTES];

static void send_voice_pkt(uint8_t flags, const uint8_t *data, uint16_t len)
{
    if (len > FRAME_BYTES) {
        return;
    }
    s_payload[0] = (uint8_t)(s_tx_seq & 0xFF);
    s_payload[1] = (uint8_t)(s_tx_seq >> 8);
    s_payload[2] = flags;
    if (len > 0 && data != NULL) {
        memcpy(&s_payload[3], data, len);
    }
    net_link_send_voice(s_payload, (uint16_t)(3 + len));
    s_tx_seq++;
}

static void voice_tx_task(void *arg)
{
    (void)arg;
    while (1) {
        if (!s_tx_active) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        if (s_tx_start_pending) {
            adpcm_encoder_reset(&s_enc);
            s_tx_seq = 0;
            send_voice_pkt(VOICE_FLAG_START, NULL, 0);
            s_tx_start_pending = false;
            ESP_LOGI(TAG, "TX start");
        }

        if (audio_read_mic(s_pcm, FRAME_SAMPLES) == ESP_OK) {
            adpcm_encode(&s_enc, s_pcm, FRAME_SAMPLES, s_enc_buf);
            send_voice_pkt(0, s_enc_buf, FRAME_BYTES);
        }

        if (s_tx_stop_req) {
            send_voice_pkt(VOICE_FLAG_END, NULL, 0);
            s_tx_stop_req = false;
            s_tx_active = false;
            ESP_LOGI(TAG, "TX stop");
        }
    }
}

static void voice_rx_task(void *arg)
{
    (void)arg;
    voice_pkt_t pkt;
    while (1) {
        if (xQueueReceive(s_rx_q, &pkt, pdMS_TO_TICKS(300)) != pdTRUE) {
            if (s_rx_active) {
                s_rx_active = false;
                ESP_LOGI(TAG, "RX timeout");
            }
            continue;
        }

        if (s_tx_active || pkt.len < 3) {
            continue; /* half duplex: drop while talking */
        }

        uint8_t flags = pkt.data[2];
        const uint8_t *adp = &pkt.data[3];
        int adp_len = (int)pkt.len - 3;

        if (flags & VOICE_FLAG_START) {
            adpcm_decoder_reset(&s_dec);
            s_rx_active = true;
            ESP_LOGI(TAG, "RX start");
        }
        if (!s_rx_active) {
            continue;
        }
        if (flags & VOICE_FLAG_END) {
            s_rx_active = false;
            ESP_LOGI(TAG, "RX stop");
            continue;
        }
        if (adp_len > 0 && adp_len <= FRAME_BYTES) {
            int samples = adp_len * 2;
            adpcm_decode(&s_dec, adp, adp_len, s_pcm_out);
            audio_write_speaker(s_pcm_out, samples);
        }
    }
}

void voice_init(void)
{
    if (s_inited) {
        return;
    }
    s_rx_q = xQueueCreate(RX_QUEUE_LEN, sizeof(voice_pkt_t));
    configASSERT(s_rx_q != NULL);

    xTaskCreate(voice_tx_task, "voice_tx", 4096, NULL, 6, &s_tx_task);
    xTaskCreate(voice_rx_task, "voice_rx", 4096, NULL, 6, &s_rx_task);

    s_inited = true;
    ESP_LOGI(TAG, "voice engine ready (16 kHz IMA ADPCM, 20 ms frames)");
}

void voice_ptt_start(void)
{
    if (!s_inited || s_tx_active) {
        return;
    }
    s_rx_active = false; /* switch to TX immediately */
    s_tx_start_pending = true;
    s_tx_stop_req = false;
    s_tx_active = true;
}

void voice_ptt_stop(void)
{
    if (!s_inited || !s_tx_active) {
        return;
    }
    s_tx_stop_req = true;
}

void voice_rx_push(const uint8_t *data, uint16_t len)
{
    if (!s_inited || s_rx_q == NULL || data == NULL || len < 3) {
        return;
    }
    voice_pkt_t pkt;
    pkt.len = (len > MAX_PAYLOAD) ? MAX_PAYLOAD : len;
    memcpy(pkt.data, data, pkt.len);
    xQueueSend(s_rx_q, &pkt, 0);
}
