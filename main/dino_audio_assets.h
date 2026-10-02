#pragma once

#include <stdint.h>

#define DINO_AUDIO_CLIP_COUNT 202
#define DINO_AUDIO_SAMPLE_RATE 8000
#define DINO_AUDIO_BLOB_HEADER_BYTES 64
#define DINO_AUDIO_BLOB_SIZE 3200798U

typedef struct {
    uint32_t offset;
    uint32_t bytes;
    uint32_t samples;
} dino_audio_clip_t;

/* Absolute blob offsets include the 64-byte identity header.
 * Every clip starts with predictor/index zero; samples excludes padding. */
extern const uint8_t dino_audio_blob_header[DINO_AUDIO_BLOB_HEADER_BYTES];
extern const dino_audio_clip_t dino_audio_clips[DINO_AUDIO_CLIP_COUNT];
