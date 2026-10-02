#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Continuous IMA ADPCM stream, low nibble first, no per-block headers. */
typedef struct {
    int predictor;
    int index;
} dino_adpcm_state_t;

/* Invalid predictor/index values are rejected without changing state. */
bool dino_adpcm_init(dino_adpcm_state_t *state, int predictor, int index);

/* Returns decoded sample count (two per byte). Rejects invalid state, pointers,
 * or insufficient capacity without changing state or PCM. Byte-aligned calls
 * retain predictor/index and produce the same PCM as a single continuous call. */
size_t dino_adpcm_decode(dino_adpcm_state_t *state, const uint8_t *data,
                        size_t bytes, int16_t *pcm, size_t capacity);
