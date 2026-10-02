#include "dino_adpcm.h"

#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

static void test_init_and_reject_invalid_state(void)
{
    dino_adpcm_state_t state = {123, 4};
    assert(dino_adpcm_init(&state, 0, 0));
    assert(state.predictor == 0 && state.index == 0);
    assert(dino_adpcm_init(&state, INT16_MIN, 88));
    assert(state.predictor == INT16_MIN && state.index == 88);
    assert(dino_adpcm_init(&state, INT16_MAX, 0));
    const dino_adpcm_state_t before = state;
    assert(!dino_adpcm_init(NULL, 0, 0));
    assert(!dino_adpcm_init(&state, 0, -1));
    assert(!dino_adpcm_init(&state, 0, 89));
    assert(!dino_adpcm_init(&state, INT16_MIN - 1, 0));
    assert(!dino_adpcm_init(&state, INT16_MAX + 1, 0));
    assert(!dino_adpcm_init(&state, INT_MIN, INT_MAX));
    assert(state.predictor == before.predictor && state.index == before.index);
}

static void test_known_vector_low_nibble_first(void)
{
    /* Reference PCM from Python audioop.adpcm2lin (nibbles swapped to match
     * this stream). Covers all 16 codes without borrowing decoder logic. */
    const uint8_t data[] = {0x10, 0x32, 0x54, 0x76, 0x98, 0xba, 0xdc, 0xfe};
    const int16_t expected[] = {
        0, 1, 4, 8, 15, 27, 47, 88, 82, 66, 41, 10, -28, -84, -181, -380
    };
    int16_t pcm[16] = {0};
    dino_adpcm_state_t state;
    assert(dino_adpcm_init(&state, 0, 0));
    assert(dino_adpcm_decode(&state, data, sizeof(data), pcm, 16) == 16);
    assert(memcmp(pcm, expected, sizeof(expected)) == 0);
    assert(state.predictor == -380 && state.index == 36);
}

static void test_predictor_and_index_clamp(void)
{
    const uint8_t positive[] = {0x77, 0x77};
    const uint8_t negative[] = {0xff, 0xff};
    int16_t pcm[4];
    dino_adpcm_state_t state;
    assert(dino_adpcm_init(&state, 32760, 88));
    assert(dino_adpcm_decode(&state, positive, 2, pcm, 4) == 4);
    for (size_t i = 0; i < 4; ++i) assert(pcm[i] == INT16_MAX);
    assert(state.predictor == INT16_MAX && state.index == 88);
    assert(dino_adpcm_init(&state, -32760, 88));
    assert(dino_adpcm_decode(&state, negative, 2, pcm, 4) == 4);
    for (size_t i = 0; i < 4; ++i) assert(pcm[i] == INT16_MIN);
    assert(state.predictor == INT16_MIN && state.index == 88);
    assert(dino_adpcm_init(&state, 0, 0));
    const uint8_t zero[] = {0x00};
    assert(dino_adpcm_decode(&state, zero, 1, pcm, 4) == 2);
    assert(state.predictor == 0 && state.index == 0);
}

static void test_rejected_decode_changes_nothing(void)
{
    const uint8_t data[] = {0xf7, 0x12};
    int16_t pcm[] = {11, 22, 33, 44};
    const int16_t expected[] = {11, 22, 33, 44};
    dino_adpcm_state_t state = {123, 4};
    assert(dino_adpcm_decode(&state, data, 2, pcm, 3) == 0);
    assert(dino_adpcm_decode(&state, data, SIZE_MAX, pcm, 4) == 0);
    assert(dino_adpcm_decode(NULL, data, 2, pcm, 4) == 0);
    assert(dino_adpcm_decode(&state, NULL, 2, pcm, 4) == 0);
    assert(dino_adpcm_decode(&state, data, 2, NULL, 4) == 0);
    assert(dino_adpcm_decode(&state, NULL, 0, NULL, 0) == 0);
    assert(state.predictor == 123 && state.index == 4);
    assert(memcmp(pcm, expected, sizeof(expected)) == 0);
    const dino_adpcm_state_t invalid[] = {
        {0, -1}, {0, 89}, {INT16_MIN - 1, 0}, {INT16_MAX + 1, 0}
    };
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        state = invalid[i];
        assert(dino_adpcm_decode(&state, data, 2, pcm, 4) == 0);
        assert(state.predictor == invalid[i].predictor);
        assert(state.index == invalid[i].index);
        assert(memcmp(pcm, expected, sizeof(expected)) == 0);
    }
}

static void test_streaming_matches_one_shot(void)
{
    const uint8_t data[] = {0x08, 0xf7, 0x13, 0xa2};
    const int16_t expected[] = {994, 999, 1075, 910, 1075, 1139, 1236, 1148};
    int16_t full[8];
    int16_t chunked[8];
    dino_adpcm_state_t one, chunks;
    assert(dino_adpcm_init(&one, 1000, 20));
    assert(dino_adpcm_init(&chunks, 1000, 20));
    assert(dino_adpcm_decode(&one, data, 4, full, 8) == 8);
    for (size_t i = 0; i < 4; ++i) {
        assert(dino_adpcm_decode(&chunks, data + i, 1, chunked + i * 2, 2) == 2);
    }
    assert(memcmp(full, chunked, sizeof(full)) == 0);
    assert(memcmp(full, expected, sizeof(full)) == 0);
    assert(one.predictor == chunks.predictor && one.index == chunks.index);
}

int main(void)
{
    test_init_and_reject_invalid_state();
    test_known_vector_low_nibble_first();
    test_predictor_and_index_clamp();
    test_rejected_decode_changes_nothing();
    test_streaming_matches_one_shot();
    puts("dino ADPCM tests: PASS");
    return 0;
}
