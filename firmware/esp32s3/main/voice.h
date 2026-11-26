/**
 * @file voice.h
 * @brief Push-to-talk voice over ESP-NOW (16 kHz IMA ADPCM, 20 ms frames)
 */
#ifndef VOICE_H
#define VOICE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Create RX queue + TX/RX tasks (call after audio_init and net_link_init) */
void voice_init(void);

/** PTT pressed: start capturing + transmitting */
void voice_ptt_start(void);

/** PTT released: stop and send end packet */
void voice_ptt_stop(void);

/** Feed a received MSG_VOICE payload (called from net_link_process) */
void voice_rx_push(const uint8_t *data, uint16_t len);

#ifdef __cplusplus
}
#endif

#endif /* VOICE_H */
