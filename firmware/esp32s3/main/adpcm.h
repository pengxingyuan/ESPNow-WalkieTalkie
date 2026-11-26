/**
 * @file adpcm.h
 * @brief IMA ADPCM 4-bit encoder/decoder (16kHz mono voice)
 */
#ifndef ADPCM_H
#define ADPCM_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int16_t pred;
    int8_t index;
} adpcm_state_t;

void adpcm_encoder_reset(adpcm_state_t *st);
void adpcm_decoder_reset(adpcm_state_t *st);

/** Encode 2 samples per output byte; samples must be even count */
void adpcm_encode(adpcm_state_t *st, const int16_t *in, int samples, uint8_t *out);

/** Decode 2 samples per input byte */
void adpcm_decode(adpcm_state_t *st, const uint8_t *in, int bytes, int16_t *out);

#ifdef __cplusplus
}
#endif

#endif /* ADPCM_H */
