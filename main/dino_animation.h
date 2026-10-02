#pragma once

#include "dino_model.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define DINO_ANIMATION_WIDTH 144
#define DINO_ANIMATION_HEIGHT 88
#define DINO_ANIMATION_PIXELS (DINO_ANIMATION_WIDTH * DINO_ANIMATION_HEIGHT)
#define DINO_ANIMATION_FRAMES DINO_FRAME_COUNT
#define DINO_ANIMATION_PALETTE_SIZE 16
#define DINO_ANIMATION_FRAME_BYTES (DINO_ANIMATION_PIXELS / 2)

/* RGB565 palette shared by all eight frames; packed I4, low nibble first.
   frames points to eight consecutive DINO_ANIMATION_FRAME_BYTES blocks. */
typedef struct {
    const uint16_t *palette;
    const uint8_t *frames;
} dino_animation_asset_t;

extern const dino_animation_asset_t dino_animation_assets[DINO_COUNT];

/* capacity is measured in uint16_t pixels. Invalid indices, pointers or a
   short output buffer are rejected before changing any output pixel. */
bool dino_animation_decode(unsigned species, unsigned frame,
                           uint16_t *out, size_t capacity);
