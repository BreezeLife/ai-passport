#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define DINO_COUNT 40
#define DINO_FOUND_MASK ((UINT64_C(1) << DINO_COUNT) - 1)
#define DINO_SAVE_SIZE 24
#define DINO_SAVE_V1_SIZE 16
#define DINO_CAMP_COUNT 4
#define DINO_FRAME_COUNT 8
#define DINO_FRAME_MS 125
#define DINO_FOOTPRINTS_PER_PAGE 8
#define DINO_FOOTPRINT_PAGE_COUNT 5

#define DINO_AUDIO_FACT_BASE 40
#define DINO_AUDIO_QUESTION_BASE 120
#define DINO_AUDIO_HINT_BASE 160
#define DINO_AUDIO_CORRECT 200
#define DINO_AUDIO_WELCOME 201

typedef enum {
    DINO_PAGE_INTRO = 0,
    DINO_PAGE_CARD,
    DINO_PAGE_STORY,
    DINO_PAGE_QUIZ,
    DINO_PAGE_FEEDBACK,
    DINO_PAGE_STAMP,
    DINO_PAGE_CAMP,
    DINO_PAGE_FOOTPRINTS,
    DINO_PAGE_MOTION,
} dino_page_t;

typedef enum {
    DINO_INPUT_UP = 0,
    DINO_INPUT_DOWN,
    DINO_INPUT_OK,
    DINO_INPUT_BACK,
    DINO_INPUT_EXIT,
} dino_input_t;

typedef struct {
    dino_page_t page;
    uint8_t dinosaur;
    uint8_t story;
    uint8_t choice;
    uint8_t camp;
    uint64_t found;
    bool muted;
    bool correct;
    bool paused;
    uint8_t frame;
    uint16_t frame_ms;
    uint8_t footprints_page;
} dino_model_t;

typedef struct {
    bool changed;
    bool persist;
    /* -1: silent; 0..39: names; 40..119: facts; 120..159: questions;
       160..199: hints; 200: encouragement; 201: welcome. */
    int audio;
} dino_effect_t;

void dino_model_init(dino_model_t *model);
dino_effect_t dino_model_handle(dino_model_t *model, dino_input_t input,
                                uint8_t correct_answer);
/* Advances only an active, playing motion page; true means frame changed.
   elapsed_ms excludes paused time. A delayed caller skips to the current frame. */
bool dino_model_tick(dino_model_t *model, uint32_t elapsed_ms);

/* V2: DINO, version=2, reserved zero, muted, dinosaur, uint64 found LE,
   four zero reserved bytes, then IEEE CRC32 LE over the preceding 20 bytes.
   Only the low 40 found bits are valid. Transient playback is not persisted. */
void dino_model_save(const dino_model_t *model, uint8_t out[DINO_SAVE_SIZE]);
/* Accepts exact 24-byte V2 or legacy 16-byte V1 records. A rejected record
   leaves model untouched; accepted records start at INTRO without a write. */
bool dino_model_load(dino_model_t *model, const uint8_t *data, size_t length);
