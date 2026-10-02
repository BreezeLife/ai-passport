#include "dino_animation.h"

bool dino_animation_decode(unsigned species, unsigned frame,
                           uint16_t *out, size_t capacity)
{
    if (species >= DINO_COUNT || frame >= DINO_ANIMATION_FRAMES || !out ||
        capacity < DINO_ANIMATION_PIXELS) return false;
    const dino_animation_asset_t *asset = &dino_animation_assets[species];
    if (!asset->palette || !asset->frames) return false;

    const uint8_t *packed = asset->frames + frame * DINO_ANIMATION_FRAME_BYTES;
    for (size_t i = 0; i < DINO_ANIMATION_FRAME_BYTES; ++i) {
        out[2 * i] = asset->palette[packed[i] & 15];
        out[2 * i + 1] = asset->palette[packed[i] >> 4];
    }
    return true;
}
