/**
 * @file audio.h
 * @brief I2S + ES8311 audio driver for the 2.8" ESP32-S3 board
 */
#ifndef AUDIO_H
#define AUDIO_H

#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Init I2S + ES8311 + PA, play a short beep */
esp_err_t audio_init(void);

/** Read mono 16-bit samples from the microphone (blocks until filled) */
esp_err_t audio_read_mic(int16_t *mono, int frames);

/** Write mono 16-bit samples to the speaker (blocks until sent) */
esp_err_t audio_write_speaker(const int16_t *mono, int frames);

/** Play a short 1 kHz confirmation beep */
void audio_play_beep(void);

/** Speaker volume 0..100 */
void audio_set_volume(int percent);

#ifdef __cplusplus
}
#endif

#endif /* AUDIO_H */
