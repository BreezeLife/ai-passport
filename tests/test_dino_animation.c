#include "dino_animation.h"

#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

static const uint16_t palette[16] = {
    0x0000, 0xffff, 0xf800, 0x07e0, 0x001f, 0xffe0, 0xf81f, 0x07ff,
    0x1234, 0x5678, 0x9abc, 0xdef0, 0x5555, 0xaaaa, 0x1357, 0x2468,
};
static const uint8_t frames[DINO_ANIMATION_FRAMES * DINO_ANIMATION_FRAME_BYTES] = {
    [0] = 0x10, [1] = 0x32, [2] = 0x54, [3] = 0x76,
    [4] = 0x98, [5] = 0xba, [6] = 0xdc, [7] = 0xfe,
    [DINO_ANIMATION_FRAME_BYTES - 1] = 0x2f,
    [7 * DINO_ANIMATION_FRAME_BYTES] = 0xab,
    [8 * DINO_ANIMATION_FRAME_BYTES - 1] = 0x87,
};
/* Slots with a missing palette/frame also exercise all-or-nothing rejection. */
const dino_animation_asset_t dino_animation_assets[DINO_COUNT] = {
    [0] = {palette, frames},
    [1] = {NULL, frames},
    [2] = {palette, NULL},
    [DINO_COUNT - 1] = {palette, frames},
};

typedef struct {
    uint16_t before;
    uint16_t pixels[DINO_ANIMATION_PIXELS];
    uint16_t after;
} guarded_t;

static void check_known_nibbles_and_edges(void)
{
    guarded_t out = {.before = 0xaaaa, .after = 0xbbbb};
    assert(dino_animation_decode(0, 0, out.pixels, DINO_ANIMATION_PIXELS));
    for (unsigned i = 0; i < 16; ++i) assert(out.pixels[i] == palette[i]);
    for (unsigned i = 16; i < DINO_ANIMATION_PIXELS - 2; ++i)
        assert(out.pixels[i] == palette[0]);
    assert(out.pixels[DINO_ANIMATION_PIXELS - 2] == palette[15]);
    assert(out.pixels[DINO_ANIMATION_PIXELS - 1] == palette[2]);
    assert(out.before == 0xaaaa && out.after == 0xbbbb);
    assert(dino_animation_decode(DINO_COUNT - 1, 7, out.pixels,
                                 DINO_ANIMATION_PIXELS));
    assert(out.pixels[0] == palette[11] && out.pixels[1] == palette[10]);
    assert(out.pixels[DINO_ANIMATION_PIXELS - 2] == palette[7]);
    assert(out.pixels[DINO_ANIMATION_PIXELS - 1] == palette[8]);
    assert(out.before == 0xaaaa && out.after == 0xbbbb);
}

static void check_rejection_is_unchanged(void)
{
    guarded_t out;
    memset(&out, 0x5a, sizeof(out));
    const guarded_t expected = out;
    const unsigned bad_species[] = {DINO_COUNT, DINO_COUNT + 1, UINT_MAX, 1, 2};
    for (unsigned i = 0; i < sizeof(bad_species) / sizeof(bad_species[0]); ++i) {
        assert(!dino_animation_decode(bad_species[i], 0, out.pixels,
                                     DINO_ANIMATION_PIXELS));
        assert(memcmp(&out, &expected, sizeof(out)) == 0);
    }
    const unsigned bad_frames[] = {DINO_ANIMATION_FRAMES, 9, UINT_MAX};
    for (unsigned i = 0; i < sizeof(bad_frames) / sizeof(bad_frames[0]); ++i) {
        assert(!dino_animation_decode(0, bad_frames[i], out.pixels,
                                     DINO_ANIMATION_PIXELS));
        assert(memcmp(&out, &expected, sizeof(out)) == 0);
    }
    const size_t capacities[] = {0, 1, DINO_ANIMATION_PIXELS - 1};
    for (unsigned i = 0; i < sizeof(capacities) / sizeof(capacities[0]); ++i) {
        assert(!dino_animation_decode(0, 0, out.pixels, capacities[i]));
        assert(memcmp(&out, &expected, sizeof(out)) == 0);
    }
    assert(!dino_animation_decode(0, 0, NULL, DINO_ANIMATION_PIXELS));
    assert(memcmp(&out, &expected, sizeof(out)) == 0);
}

static void check_all_frames_and_extra_capacity(void)
{
    guarded_t out = {.before = 0xaaaa, .after = 0xbbbb};
    for (unsigned frame = 0; frame < DINO_ANIMATION_FRAMES; ++frame) {
        assert(dino_animation_decode(0, frame, out.pixels, SIZE_MAX));
        for (unsigned i = 0; i < DINO_ANIMATION_FRAME_BYTES; ++i) {
            const uint8_t packed = frames[frame * DINO_ANIMATION_FRAME_BYTES + i];
            assert(out.pixels[2 * i] == palette[packed & 15]);
            assert(out.pixels[2 * i + 1] == palette[packed >> 4]);
        }
        assert(out.before == 0xaaaa && out.after == 0xbbbb);
    }
}

int main(void)
{
    check_known_nibbles_and_edges();
    check_rejection_is_unchanged();
    check_all_frames_and_extra_capacity();
    puts("dino animation decoder: 3/3 PASS");
    return 0;
}
