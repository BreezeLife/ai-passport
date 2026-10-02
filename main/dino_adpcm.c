#include "dino_adpcm.h"

static const int steps[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31,
    34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130,
    143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449,
    494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411,
    1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660,
    4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493,
    10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385,
    24623, 27086, 29794, 32767
};

static const int index_changes[8] = {-1, -1, -1, -1, 2, 4, 6, 8};

static bool valid_state(const dino_adpcm_state_t *state)
{
    return state && state->predictor >= INT16_MIN &&
           state->predictor <= INT16_MAX && state->index >= 0 && state->index <= 88;
}

bool dino_adpcm_init(dino_adpcm_state_t *state, int predictor, int index)
{
    const dino_adpcm_state_t initial = {predictor, index};
    if (!state || !valid_state(&initial)) return false;
    *state = initial;
    return true;
}

static int16_t decode_nibble(dino_adpcm_state_t *state, unsigned int nibble)
{
    const int step = steps[state->index];
    int difference = step >> 3;
    if (nibble & 4U) difference += step;
    if (nibble & 2U) difference += step >> 1;
    if (nibble & 1U) difference += step >> 2;
    state->predictor += (nibble & 8U) ? -difference : difference;
    if (state->predictor > INT16_MAX) state->predictor = INT16_MAX;
    if (state->predictor < INT16_MIN) state->predictor = INT16_MIN;
    state->index += index_changes[nibble & 7U];
    if (state->index < 0) state->index = 0;
    if (state->index > 88) state->index = 88;
    return (int16_t)state->predictor;
}

size_t dino_adpcm_decode(dino_adpcm_state_t *state, const uint8_t *data,
                        size_t bytes, int16_t *pcm, size_t capacity)
{
    if (!valid_state(state) || bytes == 0 || !data || !pcm || bytes > capacity / 2)
        return 0;
    for (size_t i = 0; i < bytes; ++i) {
        pcm[i * 2] = decode_nibble(state, data[i] & 15U);
        pcm[i * 2 + 1] = decode_nibble(state, data[i] >> 4);
    }
    return bytes * 2;
}
