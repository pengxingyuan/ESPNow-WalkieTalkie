/**
 * @file adpcm.c
 * @brief IMA ADPCM 4-bit codec (standard step/index tables).
 */
#include "adpcm.h"

static const int16_t step_table[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
    50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230,
    253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963,
    1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327,
    3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442,
    11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794,
    32767,
};

static const int8_t index_table[16] = {
    -1, -1, -1, -1, 2, 4, 6, 8,
    -1, -1, -1, -1, 2, 4, 6, 8,
};

static int16_t clamp16(int32_t v)
{
    if (v > 32767) return 32767;
    if (v < -32768) return -32768;
    return (int16_t)v;
}

static uint8_t encode_sample(adpcm_state_t *st, int16_t sample)
{
    int32_t step = step_table[st->index];
    int32_t delta = (int32_t)sample - st->pred;
    uint8_t code = 0;

    if (delta < 0) {
        code = 8;
        delta = -delta;
    }
    if (delta >= step) {
        code |= 4;
        delta -= step;
    }
    if (delta >= (step >> 1)) {
        code |= 2;
        delta -= (step >> 1);
    }
    if (delta >= (step >> 2)) {
        code |= 1;
    }

    int32_t diff = step >> 3;
    if (code & 4) diff += step;
    if (code & 2) diff += step >> 1;
    if (code & 1) diff += step >> 2;
    if (code & 8) {
        st->pred -= (int16_t)diff;
    } else {
        st->pred += (int16_t)diff;
    }
    st->pred = clamp16(st->pred);
    st->index += index_table[code];
    if (st->index < 0) st->index = 0;
    if (st->index > 88) st->index = 88;

    return code & 0x0F;
}

static int16_t decode_sample(adpcm_state_t *st, uint8_t code)
{
    int32_t step = step_table[st->index];
    int32_t diff = step >> 3;

    if (code & 4) diff += step;
    if (code & 2) diff += step >> 1;
    if (code & 1) diff += step >> 2;
    if (code & 8) {
        st->pred -= (int16_t)diff;
    } else {
        st->pred += (int16_t)diff;
    }
    st->pred = clamp16(st->pred);
    st->index += index_table[code];
    if (st->index < 0) st->index = 0;
    if (st->index > 88) st->index = 88;

    return st->pred;
}

void adpcm_encoder_reset(adpcm_state_t *st)
{
    st->pred = 0;
    st->index = 0;
}

void adpcm_decoder_reset(adpcm_state_t *st)
{
    st->pred = 0;
    st->index = 0;
}

void adpcm_encode(adpcm_state_t *st, const int16_t *in, int samples, uint8_t *out)
{
    for (int i = 0; i + 1 < samples; i += 2) {
        uint8_t hi = encode_sample(st, in[i]);
        uint8_t lo = encode_sample(st, in[i + 1]);
        *out++ = (uint8_t)((hi << 4) | lo);
    }
}

void adpcm_decode(adpcm_state_t *st, const uint8_t *in, int bytes, int16_t *out)
{
    for (int i = 0; i < bytes; i++) {
        uint8_t b = in[i];
        *out++ = decode_sample(st, (b >> 4) & 0x0F);
        *out++ = decode_sample(st, b & 0x0F);
    }
}
