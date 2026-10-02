#include "dino_model.h"

#include <string.h>

void dino_model_init(dino_model_t *model) {
    if (model) memset(model, 0, sizeof(*model));
}

static void enter_card(dino_model_t *model) {
    model->page = DINO_PAGE_CARD;
    model->story = 0;
    model->choice = 0;
    model->correct = false;
}

static bool same_model(const dino_model_t *a, const dino_model_t *b) {
    return a->page == b->page && a->dinosaur == b->dinosaur &&
        a->story == b->story && a->choice == b->choice &&
        a->camp == b->camp && a->found == b->found &&
        a->muted == b->muted && a->correct == b->correct &&
        a->paused == b->paused && a->frame == b->frame && a->frame_ms == b->frame_ms &&
        a->footprints_page == b->footprints_page;
}

static bool valid_model(const dino_model_t *model) {
    return model && (unsigned)model->page <= DINO_PAGE_MOTION &&
        model->dinosaur < DINO_COUNT && model->story < 3 &&
        model->choice < 2 && model->camp < DINO_CAMP_COUNT &&
        model->frame < DINO_FRAME_COUNT && model->frame_ms < DINO_FRAME_MS &&
        model->footprints_page < DINO_FOOTPRINT_PAGE_COUNT &&
        (model->found & ~DINO_FOUND_MASK) == 0;
}

bool dino_model_tick(dino_model_t *model, uint32_t elapsed_ms) {
    if (!valid_model(model) || model->page != DINO_PAGE_MOTION || model->paused)
        return false;
    const uint8_t before = model->frame;
    const uint64_t elapsed = (uint64_t)model->frame_ms + elapsed_ms;
    model->frame = (uint8_t)((model->frame + elapsed / DINO_FRAME_MS) % DINO_FRAME_COUNT);
    model->frame_ms = (uint16_t)(elapsed % DINO_FRAME_MS);
    return before != model->frame;
}

dino_effect_t dino_model_handle(dino_model_t *model, dino_input_t input,
                                uint8_t correct_answer) {
    dino_effect_t effect = { .audio = -1 };
    if (!valid_model(model)) return effect;
    const dino_model_t before = *model;
    const bool move = input == DINO_INPUT_UP || input == DINO_INPUT_DOWN;
    if (input == DINO_INPUT_EXIT) {
        if (model->page == DINO_PAGE_MOTION) {
            model->page = DINO_PAGE_CAMP;
            model->camp = 2;
        }
    } else if (input == DINO_INPUT_BACK) {
        if (model->page == DINO_PAGE_MOTION) {
            model->frame = 0;
            model->frame_ms = 0;
            model->paused = false;
        } else if (model->page == DINO_PAGE_CARD) {
            model->page = DINO_PAGE_CAMP;
            model->camp = 0;
        } else if (model->page != DINO_PAGE_INTRO) {
            enter_card(model);
        }
    } else {
        switch (model->page) {
        case DINO_PAGE_INTRO:
            if (input == DINO_INPUT_OK) {
                enter_card(model);
                effect.audio = model->dinosaur;
            }
            break;
        case DINO_PAGE_CARD:
            if (move) {
                model->dinosaur = (uint8_t)((model->dinosaur +
                    (input == DINO_INPUT_UP ? DINO_COUNT - 1 : 1)) % DINO_COUNT);
                effect.audio = model->dinosaur;
            } else if (input == DINO_INPUT_OK) {
                model->page = DINO_PAGE_STORY;
                model->story = 0;
                effect.audio = DINO_AUDIO_FACT_BASE + model->dinosaur * 2;
            }
            break;
        case DINO_PAGE_STORY:
            if (move) {
                model->story = (uint8_t)((model->story +
                    (input == DINO_INPUT_UP ? 2 : 1)) % 3);
                if (model->story < 2)
                    effect.audio = DINO_AUDIO_FACT_BASE + model->dinosaur * 2 + model->story;
            } else if (input == DINO_INPUT_OK) {
                if (model->story < 2) {
                    effect.audio = DINO_AUDIO_FACT_BASE + model->dinosaur * 2 + model->story;
                } else {
                    model->page = DINO_PAGE_QUIZ;
                    model->choice = 0;
                    effect.audio = DINO_AUDIO_QUESTION_BASE + model->dinosaur;
                }
            }
            break;
        case DINO_PAGE_QUIZ:
            if (move) {
                model->choice = (uint8_t)(1 - model->choice);
            } else if (input == DINO_INPUT_OK && correct_answer < 2) {
                model->correct = model->choice == correct_answer;
                model->page = DINO_PAGE_FEEDBACK;
                if (model->correct) {
                    model->found |= UINT64_C(1) << model->dinosaur;
                    effect.audio = DINO_AUDIO_CORRECT;
                } else {
                    effect.audio = DINO_AUDIO_HINT_BASE + model->dinosaur;
                }
            }
            break;
        case DINO_PAGE_FEEDBACK:
            if (input == DINO_INPUT_OK) {
                if (model->correct) {
                    model->page = DINO_PAGE_STAMP;
                } else {
                    model->page = DINO_PAGE_QUIZ;
                    model->choice = 0;
                    effect.audio = DINO_AUDIO_QUESTION_BASE + model->dinosaur;
                }
            }
            break;
        case DINO_PAGE_STAMP:
            if (input == DINO_INPUT_OK) {
                model->dinosaur = (uint8_t)((model->dinosaur + 1) % DINO_COUNT);
                enter_card(model);
                effect.audio = model->dinosaur;
            }
            break;
        case DINO_PAGE_CAMP:
            if (move) {
                model->camp = (uint8_t)((model->camp +
                    (input == DINO_INPUT_UP ? DINO_CAMP_COUNT - 1 : 1)) % DINO_CAMP_COUNT);
            } else if (input == DINO_INPUT_OK) {
                if (model->camp == 0) {
                    model->page = DINO_PAGE_FOOTPRINTS;
                    model->footprints_page = 0;
                }
                else if (model->camp == 1) model->muted = !model->muted;
                else if (model->camp == 2) {
                    model->page = DINO_PAGE_MOTION;
                    model->frame = 0;
                    model->frame_ms = 0;
                    model->paused = false;
                }
                else enter_card(model);
            }
            break;
        case DINO_PAGE_FOOTPRINTS:
            if (move) {
                model->footprints_page = (uint8_t)((model->footprints_page +
                    (input == DINO_INPUT_UP ? DINO_FOOTPRINT_PAGE_COUNT - 1 : 1)) %
                    DINO_FOOTPRINT_PAGE_COUNT);
            } else if (input == DINO_INPUT_OK) enter_card(model);
            break;
        case DINO_PAGE_MOTION:
            if (move) {
                model->dinosaur = (uint8_t)((model->dinosaur +
                    (input == DINO_INPUT_UP ? DINO_COUNT - 1 : 1)) % DINO_COUNT);
                model->frame = 0;
                model->frame_ms = 0;
            } else if (input == DINO_INPUT_OK) model->paused = !model->paused;
            break;
        }
    }
    effect.changed = !same_model(model, &before);
    effect.persist = model->dinosaur != before.dinosaur ||
                     model->found != before.found || model->muted != before.muted;
    if (model->muted) effect.audio = -1;
    return effect;
}

static uint32_t crc32(const uint8_t *data, size_t length) {
    uint32_t crc = UINT32_MAX;
    for (size_t i = 0; i < length; ++i) {
        crc ^= data[i];
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ ((crc & 1) ? UINT32_C(0xedb88320) : 0);
    }
    return ~crc;
}

void dino_model_save(const dino_model_t *model, uint8_t out[DINO_SAVE_SIZE]) {
    if (!model || !out) return;
    memset(out, 0, DINO_SAVE_SIZE);
    memcpy(out, "DINO", 4);
    out[4] = 2;
    out[6] = model->muted ? 1 : 0;
    out[7] = model->dinosaur;
    for (unsigned i = 0; i < 8; ++i) out[8 + i] = (uint8_t)(model->found >> (i * 8));
    const uint32_t crc = crc32(out, DINO_SAVE_SIZE - 4);
    for (unsigned i = 0; i < 4; ++i)
        out[DINO_SAVE_SIZE - 4 + i] = (uint8_t)(crc >> (i * 8));
}

bool dino_model_load(dino_model_t *model, const uint8_t *data, size_t length) {
    if (!model || !data ||
        (length != DINO_SAVE_SIZE && length != DINO_SAVE_V1_SIZE)) return false;
    if (memcmp(data, "DINO", 4) != 0 || data[6] > 1) return false;
    uint32_t stored_crc = 0;
    for (unsigned i = 0; i < 4; ++i)
        stored_crc |= (uint32_t)data[length - 4 + i] << (i * 8);
    if (stored_crc != crc32(data, length - 4)) return false;

    dino_model_t loaded;
    dino_model_init(&loaded);
    if (length == DINO_SAVE_V1_SIZE) {
        if (data[4] != 1 || data[7] >= 8) return false;
        for (unsigned i = 8; i < 12; ++i) if (data[i] != 0) return false;
        loaded.found = data[5];
    } else {
        if (data[4] != 2 || data[5] != 0 || data[7] >= DINO_COUNT) return false;
        for (unsigned i = 16; i < 20; ++i) if (data[i] != 0) return false;
        for (unsigned i = 0; i < 8; ++i)
            loaded.found |= (uint64_t)data[8 + i] << (i * 8);
        if (loaded.found & ~DINO_FOUND_MASK) return false;
    }
    loaded.muted = data[6] != 0;
    loaded.dinosaur = data[7];
    *model = loaded;
    return true;
}
