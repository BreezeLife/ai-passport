#include "dino_model.h"

#include <stdio.h>
#include <string.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); \
        return false; \
    } \
} while (0)

static bool same_model(const dino_model_t *a, const dino_model_t *b) {
    return a->page == b->page && a->dinosaur == b->dinosaur &&
        a->story == b->story && a->choice == b->choice &&
        a->camp == b->camp && a->found == b->found &&
        a->muted == b->muted && a->correct == b->correct &&
        a->paused == b->paused && a->frame == b->frame && a->frame_ms == b->frame_ms &&
        a->footprints_page == b->footprints_page;
}

static bool test_intro_and_card_navigation(void) {
    dino_model_t model;
    dino_model_init(&model);
    CHECK(model.page == DINO_PAGE_INTRO);
    CHECK(model.dinosaur == 0 && model.found == 0 && !model.muted);
    dino_effect_t effect = dino_model_handle(&model, DINO_INPUT_BACK, 0);
    CHECK(!effect.changed && !effect.persist && effect.audio == -1);
    effect = dino_model_handle(&model, DINO_INPUT_OK, 0);
    CHECK(model.page == DINO_PAGE_CARD && effect.changed);
    CHECK(!effect.persist && effect.audio == 0);
    effect = dino_model_handle(&model, DINO_INPUT_UP, 0);
    CHECK(model.dinosaur == DINO_COUNT - 1 && effect.audio == DINO_COUNT - 1);
    CHECK(effect.changed && effect.persist);
    effect = dino_model_handle(&model, DINO_INPUT_DOWN, 0);
    CHECK(model.dinosaur == 0 && effect.audio == 0 && effect.persist);
    for (unsigned i = 0; i < DINO_COUNT; ++i) {
        effect = dino_model_handle(&model, DINO_INPUT_DOWN, 0);
        CHECK(model.dinosaur == (i + 1) % DINO_COUNT);
        CHECK(effect.audio == model.dinosaur && effect.persist);
    }
    CHECK(model.dinosaur == 0 && model.found == 0);
    return true;
}

static bool test_story_navigation_and_replay(void) {
    for (uint8_t dinosaur = 0; dinosaur < DINO_COUNT; ++dinosaur) {
        dino_model_t model = { .page = DINO_PAGE_CARD, .dinosaur = dinosaur };
        dino_effect_t effect = dino_model_handle(&model, DINO_INPUT_OK, 0);
        CHECK(model.page == DINO_PAGE_STORY && model.story == 0);
        CHECK(effect.changed && !effect.persist && effect.audio == DINO_AUDIO_FACT_BASE + dinosaur * 2);
        effect = dino_model_handle(&model, DINO_INPUT_OK, 0);
        CHECK(!effect.changed && effect.audio == DINO_AUDIO_FACT_BASE + dinosaur * 2);
        effect = dino_model_handle(&model, DINO_INPUT_UP, 0);
        CHECK(model.story == 2 && effect.changed && effect.audio == -1);
        effect = dino_model_handle(&model, DINO_INPUT_UP, 0);
        CHECK(model.story == 1 && effect.audio == DINO_AUDIO_FACT_BASE + 1 + dinosaur * 2);
        effect = dino_model_handle(&model, DINO_INPUT_OK, 0);
        CHECK(model.story == 1 && !effect.changed && effect.audio == DINO_AUDIO_FACT_BASE + 1 + dinosaur * 2);
        effect = dino_model_handle(&model, DINO_INPUT_DOWN, 0);
        CHECK(model.story == 2 && effect.audio == -1);
        effect = dino_model_handle(&model, DINO_INPUT_DOWN, 0);
        CHECK(model.story == 0 && effect.audio == DINO_AUDIO_FACT_BASE + dinosaur * 2);
        (void)dino_model_handle(&model, DINO_INPUT_UP, 0);
        effect = dino_model_handle(&model, DINO_INPUT_OK, 0);
        CHECK(model.page == DINO_PAGE_QUIZ && model.choice == 0);
        CHECK(effect.audio == DINO_AUDIO_QUESTION_BASE + dinosaur && !effect.persist);
    }
    return true;
}

static bool test_all_dinosaurs_can_collect_footprints(void) {
    dino_model_t model;
    dino_model_init(&model);
    (void)dino_model_handle(&model, DINO_INPUT_OK, 0);
    for (uint8_t dinosaur = 0; dinosaur < DINO_COUNT; ++dinosaur) {
        CHECK(model.page == DINO_PAGE_CARD && model.dinosaur == dinosaur);
        (void)dino_model_handle(&model, DINO_INPUT_OK, 0);
        (void)dino_model_handle(&model, DINO_INPUT_UP, 0);
        (void)dino_model_handle(&model, DINO_INPUT_OK, 0);
        const uint8_t answer = dinosaur % 2;
        if (answer) (void)dino_model_handle(&model, DINO_INPUT_DOWN, answer);
        dino_effect_t effect = dino_model_handle(&model, DINO_INPUT_OK, answer);
        CHECK(model.page == DINO_PAGE_FEEDBACK && model.correct);
        CHECK(model.found == ((UINT64_C(1) << (dinosaur + 1)) - 1));
        CHECK(effect.changed && effect.persist && effect.audio == DINO_AUDIO_CORRECT);
        effect = dino_model_handle(&model, DINO_INPUT_OK, answer);
        CHECK(model.page == DINO_PAGE_STAMP && effect.changed);
        CHECK(!effect.persist && effect.audio == -1);
        effect = dino_model_handle(&model, DINO_INPUT_OK, answer);
        CHECK(model.page == DINO_PAGE_CARD);
        CHECK(model.dinosaur == (dinosaur + 1) % DINO_COUNT);
        CHECK(effect.changed && effect.persist && effect.audio == model.dinosaur);
    }
    CHECK(model.found == DINO_FOUND_MASK && model.dinosaur == 0);
    return true;
}

static bool test_wrong_answers_offer_retry_without_penalty(void) {
    for (uint8_t dinosaur = 0; dinosaur < DINO_COUNT; ++dinosaur) {
        const uint8_t answer = dinosaur % 2;
        dino_model_t model = { .page = DINO_PAGE_QUIZ, .dinosaur = dinosaur,
            .choice = (uint8_t)(1 - answer), .found = 0xa5 };
        dino_effect_t effect = dino_model_handle(&model, DINO_INPUT_OK, answer);
        CHECK(model.page == DINO_PAGE_FEEDBACK && !model.correct);
        CHECK(model.found == 0xa5 && !effect.persist && effect.audio == DINO_AUDIO_HINT_BASE + dinosaur);
        effect = dino_model_handle(&model, DINO_INPUT_OK, answer);
        CHECK(model.page == DINO_PAGE_QUIZ && model.choice == 0);
        CHECK(effect.changed && !effect.persist && effect.audio == DINO_AUDIO_QUESTION_BASE + dinosaur);
        if (answer) (void)dino_model_handle(&model, DINO_INPUT_UP, answer);
        effect = dino_model_handle(&model, DINO_INPUT_OK, answer);
        CHECK(model.correct && model.page == DINO_PAGE_FEEDBACK);
        CHECK(model.found == (UINT64_C(0xa5) | (UINT64_C(1) << dinosaur)) && effect.audio == DINO_AUDIO_CORRECT);
        CHECK(effect.persist == ((UINT64_C(0xa5) & (UINT64_C(1) << dinosaur)) == 0));
    }
    return true;
}

static bool test_repeat_collection_avoids_storage_writes(void) {
    for (uint8_t dinosaur = 0; dinosaur < DINO_COUNT; ++dinosaur) {
        dino_model_t model = { .page = DINO_PAGE_QUIZ, .dinosaur = dinosaur,
            .found = DINO_FOUND_MASK };
        dino_effect_t effect = dino_model_handle(&model, DINO_INPUT_OK, 0);
        CHECK(model.correct && model.found == DINO_FOUND_MASK);
        CHECK(effect.changed && !effect.persist && effect.audio == DINO_AUDIO_CORRECT);
    }
    return true;
}

static bool test_quiz_choices_wrap_both_directions(void) {
    dino_model_t model = { .page = DINO_PAGE_QUIZ };
    dino_effect_t effect = dino_model_handle(&model, DINO_INPUT_UP, 0);
    CHECK(model.choice == 1 && effect.changed && !effect.persist && effect.audio == -1);
    effect = dino_model_handle(&model, DINO_INPUT_DOWN, 0);
    CHECK(model.choice == 0 && effect.changed && effect.audio == -1);
    (void)dino_model_handle(&model, DINO_INPUT_DOWN, 0);
    CHECK(model.choice == 1);
    (void)dino_model_handle(&model, DINO_INPUT_UP, 0);
    CHECK(model.choice == 0);
    return true;
}

static bool test_camp_preferences_and_footprint_return(void) {
    dino_model_t model = { .page = DINO_PAGE_CARD, .dinosaur = 6, .found = 0x83 };
    dino_effect_t effect = dino_model_handle(&model, DINO_INPUT_BACK, 0);
    CHECK(model.page == DINO_PAGE_CAMP && model.camp == 0);
    CHECK(effect.changed && !effect.persist && effect.audio == -1);
    effect = dino_model_handle(&model, DINO_INPUT_UP, 0);
    CHECK(model.camp == 3 && effect.changed && !effect.persist);
    effect = dino_model_handle(&model, DINO_INPUT_DOWN, 0);
    CHECK(model.camp == 0 && effect.changed);
    effect = dino_model_handle(&model, DINO_INPUT_OK, 0);
    CHECK(model.page == DINO_PAGE_FOOTPRINTS && effect.changed);
    CHECK(model.found == 0x83 && model.dinosaur == 6 && !effect.persist);
    effect = dino_model_handle(&model, DINO_INPUT_OK, 0);
    CHECK(model.page == DINO_PAGE_CARD && effect.changed && effect.audio == -1);
    (void)dino_model_handle(&model, DINO_INPUT_BACK, 0);
    (void)dino_model_handle(&model, DINO_INPUT_DOWN, 0);
    effect = dino_model_handle(&model, DINO_INPUT_OK, 0);
    CHECK(model.page == DINO_PAGE_CAMP && model.muted);
    CHECK(effect.changed && effect.persist && effect.audio == -1);
    effect = dino_model_handle(&model, DINO_INPUT_OK, 0);
    CHECK(!model.muted && effect.persist && effect.audio == -1);
    (void)dino_model_handle(&model, DINO_INPUT_DOWN, 0);
    (void)dino_model_handle(&model, DINO_INPUT_DOWN, 0);
    effect = dino_model_handle(&model, DINO_INPUT_OK, 0);
    CHECK(model.page == DINO_PAGE_CARD && model.dinosaur == 6);
    CHECK(effect.changed && !effect.persist && effect.audio == -1);
    return true;
}

static bool test_long_return_preserves_dinosaur_and_footprints(void) {
    for (dino_page_t page = DINO_PAGE_INTRO; page <= DINO_PAGE_FOOTPRINTS; ++page) {
        dino_model_t model = { .page = page, .dinosaur = 5, .found = 0x62,
            .story = 1, .choice = 1, .camp = 2, .correct = true };
        const dino_model_t before = model;
        dino_effect_t effect = dino_model_handle(&model, DINO_INPUT_BACK, 0);
        CHECK(model.dinosaur == 5 && model.found == 0x62 && !effect.persist);
        CHECK(effect.audio == -1);
        if (page == DINO_PAGE_INTRO) {
            CHECK(same_model(&model, &before) && !effect.changed);
        } else {
            CHECK(model.page == (page == DINO_PAGE_CARD ? DINO_PAGE_CAMP : DINO_PAGE_CARD));
            CHECK(effect.changed);
        }
    }
    return true;
}

static bool test_muted_actions_suppress_every_narration(void) {
    const dino_model_t cases[] = {
        { .page = DINO_PAGE_INTRO, .muted = true },
        { .page = DINO_PAGE_CARD, .muted = true },
        { .page = DINO_PAGE_STORY, .muted = true },
        { .page = DINO_PAGE_STORY, .story = 1, .muted = true },
        { .page = DINO_PAGE_STORY, .story = 2, .muted = true },
        { .page = DINO_PAGE_QUIZ, .muted = true },
        { .page = DINO_PAGE_QUIZ, .choice = 1, .muted = true },
        { .page = DINO_PAGE_FEEDBACK, .muted = true },
        { .page = DINO_PAGE_STAMP, .muted = true },
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        dino_model_t model = cases[i];
        dino_effect_t effect = dino_model_handle(&model, DINO_INPUT_OK, 0);
        CHECK(effect.audio == -1 && model.muted);
    }
    dino_model_t model = { .page = DINO_PAGE_CARD, .muted = true };
    CHECK(dino_model_handle(&model, DINO_INPUT_DOWN, 0).audio == -1);
    model.page = DINO_PAGE_STORY;
    CHECK(dino_model_handle(&model, DINO_INPUT_DOWN, 0).audio == -1);
    return true;
}

static bool test_ignored_inputs_and_invalid_arguments(void) {
    const dino_page_t pages[] = { DINO_PAGE_INTRO, DINO_PAGE_FEEDBACK,
        DINO_PAGE_STAMP };
    for (size_t i = 0; i < sizeof(pages) / sizeof(pages[0]); ++i) {
        dino_model_t model = { .page = pages[i], .dinosaur = 3 };
        const dino_model_t before = model;
        dino_effect_t effect = dino_model_handle(&model, DINO_INPUT_UP, 0);
        CHECK(same_model(&model, &before));
        CHECK(!effect.changed && !effect.persist && effect.audio == -1);
        effect = dino_model_handle(&model, DINO_INPUT_EXIT, 0);
        CHECK(same_model(&model, &before));
        CHECK(!effect.changed && !effect.persist && effect.audio == -1);
        effect = dino_model_handle(&model, DINO_INPUT_DOWN, 0);
        CHECK(same_model(&model, &before));
        CHECK(!effect.changed && !effect.persist && effect.audio == -1);
    }
    dino_effect_t effect = dino_model_handle(NULL, DINO_INPUT_OK, 0);
    CHECK(!effect.changed && !effect.persist && effect.audio == -1);
    dino_model_init(NULL);
    dino_model_t model = { .page = DINO_PAGE_CARD };
    const dino_model_t before = model;
    effect = dino_model_handle(&model, (dino_input_t)99, 0);
    CHECK(same_model(&model, &before) && !effect.changed && effect.audio == -1);
    model.page = DINO_PAGE_QUIZ;
    effect = dino_model_handle(&model, DINO_INPUT_OK, 2);
    CHECK(model.page == DINO_PAGE_QUIZ && !effect.changed && effect.audio == -1);
    return true;
}

static uint32_t record_crc32(const uint8_t *data, size_t length) {
    uint32_t crc = UINT32_MAX;
    for (size_t i = 0; i < length; ++i) {
        crc ^= data[i];
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ ((crc & 1) ? UINT32_C(0xedb88320) : 0);
    }
    return ~crc;
}

static void update_crc(uint8_t *record, size_t length) {
    const uint32_t crc = record_crc32(record, length - 4);
    for (unsigned i = 0; i < 4; ++i) record[length - 4 + i] = (uint8_t)(crc >> (i * 8));
}

static void update_record_crc(uint8_t record[DINO_SAVE_SIZE]) {
    update_crc(record, DINO_SAVE_SIZE);
}

static bool test_save_has_canonical_format(void) {
    const dino_model_t model = { .page = DINO_PAGE_QUIZ, .dinosaur = 39,
        .story = 2, .choice = 1, .camp = 2, .found = UINT64_C(0x80402010a5),
        .muted = true, .correct = true };
    uint8_t record[DINO_SAVE_SIZE];
    dino_model_save(&model, record);
    CHECK(memcmp(record, "DINO", 4) == 0);
    CHECK(record[4] == 2 && record[5] == 0 && record[6] == 1 && record[7] == 39);
    const uint8_t found[] = { 0xa5, 0x10, 0x20, 0x40, 0x80, 0, 0, 0 };
    CHECK(memcmp(record + 8, found, sizeof(found)) == 0);
    for (unsigned i = 16; i < 20; ++i) CHECK(record[i] == 0);
    CHECK(record_crc32(record, 20) == UINT32_C(0x871a9e89));
    CHECK(record[20] == 0x89 && record[21] == 0x9e &&
          record[22] == 0x1a && record[23] == 0x87);
    dino_model_save(NULL, record);
    CHECK(record[4] == 2 && record[8] == 0xa5);
    dino_model_save(&model, NULL);
    return true;
}

static bool test_representative_persisted_states_roundtrip(void) {
    for (unsigned found = 0; found <= UINT8_MAX; ++found) {
        for (uint8_t dinosaur = 0; dinosaur < DINO_COUNT; ++dinosaur) {
            for (unsigned muted = 0; muted < 2; ++muted) {
                const dino_model_t original = { .page = DINO_PAGE_FEEDBACK,
                    .dinosaur = dinosaur, .story = 2, .choice = 1,
                    .camp = 3, .found = found | (UINT64_C(1) << dinosaur),
                    .muted = muted != 0, .correct = true, .paused = true, .frame = 7, .frame_ms = 124 };
                uint8_t record[DINO_SAVE_SIZE];
                dino_model_save(&original, record);
                dino_model_t loaded = { .page = DINO_PAGE_CAMP };
                CHECK(dino_model_load(&loaded, record, sizeof(record)));
                CHECK(loaded.page == DINO_PAGE_INTRO);
                CHECK(loaded.dinosaur == dinosaur && loaded.found == original.found);
                CHECK(loaded.muted == original.muted);
                CHECK(loaded.story == 0 && loaded.choice == 0 && loaded.camp == 0 && !loaded.correct &&
                      !loaded.paused && loaded.frame == 0 && loaded.frame_ms == 0);
                dino_effect_t effect = dino_model_handle(&loaded, DINO_INPUT_OK, 0);
                CHECK(loaded.page == DINO_PAGE_CARD && effect.changed && !effect.persist);
                CHECK(effect.audio == (original.muted ? -1 : dinosaur));
            }
        }
    }
    return true;
}

static bool rejected_without_mutation(const uint8_t *record, size_t length) {
    dino_model_t model = { .page = DINO_PAGE_FOOTPRINTS, .dinosaur = 6,
        .story = 2, .choice = 1, .camp = 1, .found = 0xb3,
        .muted = true, .correct = true };
    const dino_model_t before = model;
    CHECK(!dino_model_load(&model, record, length));
    CHECK(same_model(&model, &before));
    return true;
}

static bool test_any_single_bit_corruption_is_rejected(void) {
    const dino_model_t model = { .dinosaur = 4, .found = 0xa5, .muted = true };
    uint8_t original[DINO_SAVE_SIZE];
    dino_model_save(&model, original);
    dino_model_t baseline;
    CHECK(dino_model_load(&baseline, original, sizeof(original)));
    for (unsigned bit = 0; bit < DINO_SAVE_SIZE * 8; ++bit) {
        uint8_t damaged[DINO_SAVE_SIZE];
        memcpy(damaged, original, sizeof(damaged));
        damaged[bit / 8] ^= (uint8_t)(1u << (bit % 8));
        CHECK(rejected_without_mutation(damaged, sizeof(damaged)));
    }
    return true;
}

static bool test_invalid_fields_rejected_even_with_valid_crc(void) {
    const dino_model_t model = { .dinosaur = 4, .found = 0xa5, .muted = true };
    uint8_t original[DINO_SAVE_SIZE];
    dino_model_save(&model, original);
    dino_model_t baseline;
    CHECK(dino_model_load(&baseline, original, sizeof(original)));
    const struct { unsigned offset; uint8_t value; } invalid[] = {
        { 0, 'd' }, { 1, 'i' }, { 2, 'n' }, { 3, 'o' },
        { 4, 0 }, { 4, 1 }, { 4, 3 }, { 4, UINT8_MAX }, { 5, 1 },
        { 6, 2 }, { 6, UINT8_MAX },
        { 7, DINO_COUNT }, { 7, UINT8_MAX },
        { 13, 1 }, { 14, 1 }, { 15, 1 },
        { 16, 1 }, { 17, 1 }, { 18, 1 }, { 19, 1 },
    };
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        uint8_t damaged[DINO_SAVE_SIZE];
        memcpy(damaged, original, sizeof(damaged));
        damaged[invalid[i].offset] = invalid[i].value;
        update_record_crc(damaged);
        CHECK(rejected_without_mutation(damaged, sizeof(damaged)));
    }
    for (unsigned bit = DINO_COUNT; bit < 64; ++bit) {
        uint8_t damaged[DINO_SAVE_SIZE];
        memcpy(damaged, original, sizeof(damaged));
        damaged[8 + bit / 8] |= (uint8_t)(1u << (bit % 8));
        update_record_crc(damaged);
        CHECK(rejected_without_mutation(damaged, sizeof(damaged)));
    }
    return true;
}

static bool test_load_requires_exact_record_length(void) {
    const dino_model_t model = { .dinosaur = 4, .found = 0xa5 };
    uint8_t record[DINO_SAVE_SIZE * 2] = { 0 };
    dino_model_save(&model, record);
    dino_model_t baseline;
    CHECK(dino_model_load(&baseline, record, DINO_SAVE_SIZE));
    for (size_t length = 0; length <= sizeof(record); ++length) {
        if (length != DINO_SAVE_SIZE) CHECK(rejected_without_mutation(record, length));
    }
    CHECK(rejected_without_mutation(NULL, DINO_SAVE_SIZE));
    CHECK(rejected_without_mutation(NULL, 0));
    CHECK(!dino_model_load(NULL, record, DINO_SAVE_SIZE));
    memset(record, 0, sizeof(record));
    CHECK(rejected_without_mutation(record, DINO_SAVE_SIZE));
    return true;
}

static bool test_invalid_runtime_state_is_ignored(void) {
    const dino_model_t invalid[] = {
        { .page = (dino_page_t)-1 },
        { .page = (dino_page_t)(DINO_PAGE_MOTION + 1) },
        { .page = DINO_PAGE_QUIZ, .dinosaur = DINO_COUNT },
        { .page = DINO_PAGE_STORY, .story = 3 },
        { .page = DINO_PAGE_QUIZ, .choice = 2 },
        { .page = DINO_PAGE_CAMP, .camp = DINO_CAMP_COUNT },
        { .page = DINO_PAGE_MOTION, .frame = DINO_FRAME_COUNT },
        { .page = DINO_PAGE_MOTION, .frame_ms = DINO_FRAME_MS },
        { .page = DINO_PAGE_QUIZ, .found = UINT64_C(1) << DINO_COUNT },
        { .page = DINO_PAGE_FOOTPRINTS, .footprints_page = DINO_FOOTPRINT_PAGE_COUNT },
    };
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        for (dino_input_t input = DINO_INPUT_UP; input <= DINO_INPUT_EXIT; ++input) {
            dino_model_t model = invalid[i];
            dino_effect_t effect = dino_model_handle(&model, input, 0);
            CHECK(same_model(&model, &invalid[i]));
            CHECK(!effect.changed && !effect.persist && effect.audio == -1);
        }
    }
    return true;
}

static bool test_large_event_sequences_preserve_invariants(void) {
    uint32_t random = UINT32_C(0x42c0ffee);
    for (unsigned sequence = 0; sequence < 64; ++sequence) {
        dino_model_t model;
        dino_model_init(&model);
        for (unsigned event = 0; event < 4096; ++event) {
            random = random * UINT32_C(1664525) + UINT32_C(1013904223);
            const dino_input_t input = (dino_input_t)((random >> 16) % 7);
            const uint8_t answer = (uint8_t)((random >> 24) & 1);
            const dino_model_t before = model;
            const dino_effect_t effect = dino_model_handle(&model, input, answer);
            CHECK(model.page >= DINO_PAGE_INTRO && model.page <= DINO_PAGE_MOTION);
            CHECK(model.dinosaur < DINO_COUNT && model.story < 3);
            CHECK(model.choice < 2 && model.camp < DINO_CAMP_COUNT);
            CHECK(model.frame < DINO_FRAME_COUNT && model.frame_ms < DINO_FRAME_MS);
            CHECK(model.footprints_page < DINO_FOOTPRINT_PAGE_COUNT);
            CHECK((model.found & ~DINO_FOUND_MASK) == 0);
            CHECK((model.found & before.found) == before.found);
            CHECK(effect.changed == !same_model(&model, &before));
            CHECK(effect.persist == (model.found != before.found ||
                  model.dinosaur != before.dinosaur || model.muted != before.muted));
            CHECK(effect.audio >= -1 && effect.audio <= DINO_AUDIO_WELCOME);
            CHECK(!model.muted || effect.audio == -1);
            if (model.page == DINO_PAGE_FEEDBACK && model.correct)
                CHECK((model.found & (UINT64_C(1) << model.dinosaur)) != 0);
            const uint8_t frame_before = model.frame;
            const uint64_t found_before = model.found;
            const bool advanced = dino_model_tick(&model, random);
            CHECK(advanced == (frame_before != model.frame));
            CHECK(model.frame < DINO_FRAME_COUNT && model.frame_ms < DINO_FRAME_MS);
            CHECK(model.found == found_before);
            if ((event % 256) == 0) {
                uint8_t record[DINO_SAVE_SIZE];
                dino_model_save(&model, record);
                dino_model_t loaded;
                CHECK(dino_model_load(&loaded, record, sizeof(record)));
                CHECK(loaded.found == model.found && loaded.dinosaur == model.dinosaur &&
                      loaded.muted == model.muted);
            }
        }
    }
    return true;
}

static bool test_footprint_pages_wrap_and_preserve_high_bits(void) {
    dino_model_t model = { .page = DINO_PAGE_CAMP, .camp = 0,
        .dinosaur = 39, .found = (UINT64_C(1) << 39) | 1, .footprints_page = 4 };
    dino_effect_t effect = dino_model_handle(&model, DINO_INPUT_OK, 0);
    CHECK(model.page == DINO_PAGE_FOOTPRINTS && model.footprints_page == 0);
    CHECK(effect.changed && !effect.persist && effect.audio == -1);
    effect = dino_model_handle(&model, DINO_INPUT_UP, 0);
    CHECK(model.footprints_page == 4 && effect.changed && !effect.persist);
    effect = dino_model_handle(&model, DINO_INPUT_DOWN, 0);
    CHECK(model.footprints_page == 0 && effect.changed && !effect.persist);
    for (unsigned i = 0; i < DINO_FOOTPRINT_PAGE_COUNT; ++i) {
        effect = dino_model_handle(&model, DINO_INPUT_DOWN, 0);
        CHECK(model.footprints_page == (i + 1) % DINO_FOOTPRINT_PAGE_COUNT);
        CHECK(!effect.persist && effect.audio == -1);
    }
    CHECK(model.found == ((UINT64_C(1) << 39) | 1) && model.dinosaur == 39);
    effect = dino_model_handle(&model, DINO_INPUT_OK, 0);
    CHECK(model.page == DINO_PAGE_CARD && model.dinosaur == 39 && !effect.persist);
    return true;
}

static bool test_camp_motion_entry_starts_playing_silently(void) {
    dino_model_t model = { .page = DINO_PAGE_CAMP, .camp = 1, .dinosaur = 39,
        .paused = true, .frame = 7, .frame_ms = 124 };
    dino_effect_t effect = dino_model_handle(&model, DINO_INPUT_DOWN, 0);
    CHECK(model.camp == 2 && effect.changed && !effect.persist);
    effect = dino_model_handle(&model, DINO_INPUT_OK, 0);
    CHECK(model.page == DINO_PAGE_MOTION && model.dinosaur == 39);
    CHECK(!model.paused && model.frame == 0 && model.frame_ms == 0);
    CHECK(effect.changed && !effect.persist && effect.audio == -1);
    return true;
}

static bool test_motion_browses_all_dinosaurs_preserving_pause(void) {
    for (unsigned paused = 0; paused < 2; ++paused) {
        dino_model_t model = { .page = DINO_PAGE_MOTION, .paused = paused != 0,
            .frame = 7, .frame_ms = 124, .found = DINO_FOUND_MASK };
        dino_effect_t effect = dino_model_handle(&model, DINO_INPUT_UP, 0);
        CHECK(model.dinosaur == DINO_COUNT - 1 && model.frame == 0 && model.frame_ms == 0);
        CHECK(model.paused == (paused != 0) && effect.changed && effect.persist && effect.audio == -1);
        for (unsigned i = 0; i < DINO_COUNT; ++i) {
            model.frame = 6;
            model.frame_ms = 50;
            effect = dino_model_handle(&model, DINO_INPUT_DOWN, 0);
            CHECK(model.dinosaur == i && model.frame == 0 && model.frame_ms == 0);
            CHECK(model.paused == (paused != 0) && model.page == DINO_PAGE_MOTION);
            CHECK(effect.changed && effect.persist && effect.audio == -1);
        }
        CHECK(model.found == DINO_FOUND_MASK);
    }
    return true;
}

static bool test_motion_pause_excludes_elapsed_time(void) {
    dino_model_t model = { .page = DINO_PAGE_MOTION };
    CHECK(!dino_model_tick(&model, 124) && model.frame_ms == 124);
    dino_effect_t effect = dino_model_handle(&model, DINO_INPUT_OK, 0);
    CHECK(model.paused && effect.changed && !effect.persist && effect.audio == -1);
    const dino_model_t paused = model;
    CHECK(!dino_model_tick(&model, UINT32_MAX));
    CHECK(same_model(&model, &paused));
    effect = dino_model_handle(&model, DINO_INPUT_OK, 0);
    CHECK(!model.paused && effect.changed && !effect.persist && effect.audio == -1);
    CHECK(dino_model_tick(&model, 1) && model.frame == 1 && model.frame_ms == 0);
    return true;
}

static bool test_motion_replay_clears_frame_and_remainder(void) {
    dino_model_t model = { .page = DINO_PAGE_MOTION, .dinosaur = 39,
        .paused = true, .frame = 7, .frame_ms = 124, .found = DINO_FOUND_MASK };
    dino_effect_t effect = dino_model_handle(&model, DINO_INPUT_BACK, 0);
    CHECK(model.page == DINO_PAGE_MOTION && model.dinosaur == 39 && !model.paused);
    CHECK(model.frame == 0 && model.frame_ms == 0 && model.found == DINO_FOUND_MASK);
    CHECK(effect.changed && !effect.persist && effect.audio == -1);
    CHECK(!dino_model_tick(&model, 124) && model.frame == 0);
    CHECK(dino_model_tick(&model, 1) && model.frame == 1);
    return true;
}

static bool test_motion_exit_returns_to_camp_selection(void) {
    dino_model_t model = { .page = DINO_PAGE_MOTION, .dinosaur = 39,
        .paused = true, .frame = 6, .frame_ms = 31, .found = UINT64_C(1) << 39 };
    dino_effect_t effect = dino_model_handle(&model, DINO_INPUT_EXIT, 0);
    CHECK(model.page == DINO_PAGE_CAMP && model.camp == 2 && model.dinosaur == 39);
    CHECK(model.found == (UINT64_C(1) << 39));
    CHECK(effect.changed && !effect.persist && effect.audio == -1);
    const dino_model_t outside = model;
    CHECK(!dino_model_tick(&model, UINT32_MAX) && same_model(&model, &outside));
    effect = dino_model_handle(&model, DINO_INPUT_OK, 0);
    CHECK(model.page == DINO_PAGE_MOTION && !model.paused && model.frame == 0 && model.frame_ms == 0);
    CHECK(effect.changed && !effect.persist);
    return true;
}

static bool test_motion_tick_boundaries_and_large_intervals(void) {
    dino_model_t model = { .page = DINO_PAGE_MOTION };
    for (unsigned i = 0; i < DINO_FRAME_COUNT; ++i) {
        CHECK(!dino_model_tick(&model, 124));
        CHECK(model.frame == i && model.frame_ms == 124);
        CHECK(dino_model_tick(&model, 1));
        CHECK(model.frame == (i + 1) % DINO_FRAME_COUNT && model.frame_ms == 0);
    }
    CHECK(!dino_model_tick(&model, DINO_FRAME_COUNT * DINO_FRAME_MS));
    CHECK(model.frame == 0 && model.frame_ms == 0);
    model.frame = 3;
    model.frame_ms = 124;
    const uint64_t elapsed = (uint64_t)UINT32_MAX + model.frame_ms;
    CHECK(dino_model_tick(&model, UINT32_MAX));
    CHECK(model.frame == (3 + elapsed / DINO_FRAME_MS) % DINO_FRAME_COUNT);
    CHECK(model.frame_ms == elapsed % DINO_FRAME_MS);
    const dino_model_t before = model;
    CHECK(!dino_model_tick(&model, 0) && same_model(&model, &before));
    return true;
}

static bool test_motion_ticks_are_independent_per_instance(void) {
    dino_model_t one = { .page = DINO_PAGE_MOTION };
    dino_model_t two = { .page = DINO_PAGE_MOTION };
    CHECK(!dino_model_tick(&one, 100) && one.frame_ms == 100);
    CHECK(!dino_model_tick(&two, 30) && two.frame_ms == 30);
    CHECK(dino_model_tick(&one, 25) && one.frame == 1 && one.frame_ms == 0);
    CHECK(!dino_model_tick(&two, 94) && two.frame == 0 && two.frame_ms == 124);
    CHECK(dino_model_tick(&two, 1) && two.frame == 1 && two.frame_ms == 0);
    return true;
}

static bool test_motion_tick_is_noop_outside_motion_or_invalid(void) {
    for (dino_page_t page = DINO_PAGE_INTRO; page <= DINO_PAGE_FOOTPRINTS; ++page) {
        dino_model_t model = { .page = page, .frame = 7, .frame_ms = 124 };
        const dino_model_t before = model;
        CHECK(!dino_model_tick(&model, UINT32_MAX) && same_model(&model, &before));
    }
    dino_model_t model = { .page = DINO_PAGE_MOTION, .frame = DINO_FRAME_COUNT };
    const dino_model_t before = model;
    CHECK(!dino_model_tick(&model, UINT32_MAX) && same_model(&model, &before));
    CHECK(!dino_model_tick(NULL, UINT32_MAX));
    return true;
}

static void make_v1(uint8_t record[DINO_SAVE_V1_SIZE], uint8_t found,
                    uint8_t dinosaur, bool muted) {
    memset(record, 0, DINO_SAVE_V1_SIZE);
    memcpy(record, "DINO", 4);
    record[4] = 1;
    record[5] = found;
    record[6] = muted ? 1 : 0;
    record[7] = dinosaur;
    update_crc(record, DINO_SAVE_V1_SIZE);
}

static bool test_v1_migration_preserves_all_original_states(void) {
    for (unsigned found = 0; found <= UINT8_MAX; ++found) {
        for (uint8_t dinosaur = 0; dinosaur < 8; ++dinosaur) {
            for (unsigned muted = 0; muted < 2; ++muted) {
                uint8_t record[DINO_SAVE_V1_SIZE], original[DINO_SAVE_V1_SIZE];
                make_v1(record, (uint8_t)found, dinosaur, muted != 0);
                memcpy(original, record, sizeof(original));
                dino_model_t model = { .page = DINO_PAGE_MOTION, .paused = true,
                    .frame = 7, .frame_ms = 124, .footprints_page = 4 };
                CHECK(dino_model_load(&model, record, sizeof(record)));
                CHECK(memcmp(record, original, sizeof(record)) == 0);
                CHECK(model.page == DINO_PAGE_INTRO && model.found == found);
                CHECK(model.dinosaur == dinosaur && model.muted == (muted != 0));
                CHECK(!model.paused && model.frame == 0 && model.frame_ms == 0 && model.footprints_page == 0);
                dino_effect_t effect = dino_model_handle(&model, DINO_INPUT_OK, 0);
                CHECK(model.page == DINO_PAGE_CARD && !effect.persist);
                CHECK(effect.audio == (muted ? -1 : dinosaur));
                effect = dino_model_handle(&model, DINO_INPUT_BACK, 0);
                CHECK(model.page == DINO_PAGE_CAMP && !effect.persist);
                uint8_t upgraded[DINO_SAVE_SIZE];
                dino_model_save(&model, upgraded);
                CHECK(upgraded[4] == 2);
                dino_model_t reloaded;
                CHECK(dino_model_load(&reloaded, upgraded, sizeof(upgraded)));
                CHECK(reloaded.found == found && reloaded.dinosaur == dinosaur &&
                      reloaded.muted == (muted != 0));
            }
        }
    }
    return true;
}

static bool test_v1_rejects_every_bit_corruption_and_invalid_cursor(void) {
    uint8_t original[DINO_SAVE_V1_SIZE];
    make_v1(original, 0xa5, 7, true);
    dino_model_t baseline;
    CHECK(dino_model_load(&baseline, original, sizeof(original)));
    for (unsigned bit = 0; bit < sizeof(original) * 8; ++bit) {
        uint8_t damaged[DINO_SAVE_V1_SIZE];
        memcpy(damaged, original, sizeof(damaged));
        damaged[bit / 8] ^= (uint8_t)(1u << (bit % 8));
        CHECK(rejected_without_mutation(damaged, sizeof(damaged)));
    }
    for (uint8_t dinosaur = 8; dinosaur < DINO_COUNT; ++dinosaur) {
        uint8_t invalid[DINO_SAVE_V1_SIZE];
        make_v1(invalid, 0xff, dinosaur, false);
        CHECK(rejected_without_mutation(invalid, sizeof(invalid)));
    }
    return true;
}

static bool test_v1_and_v2_versions_require_matching_lengths(void) {
    uint8_t record[DINO_SAVE_SIZE] = { 0 };
    make_v1(record, 0xff, 7, false);
    CHECK(rejected_without_mutation(record, DINO_SAVE_SIZE));
    record[4] = 2;
    const uint32_t crc = record_crc32(record, DINO_SAVE_V1_SIZE - 4);
    for (unsigned i = 0; i < 4; ++i) record[12 + i] = (uint8_t)(crc >> (i * 8));
    CHECK(rejected_without_mutation(record, DINO_SAVE_V1_SIZE));
    return true;
}

static bool test_v1_invalid_structure_with_valid_crc(void) {
    uint8_t original[DINO_SAVE_V1_SIZE];
    make_v1(original, 0xa5, 7, true);
    const struct { unsigned offset; uint8_t value; } invalid[] = {
        { 0, 'd' }, { 1, 'i' }, { 2, 'n' }, { 3, 'o' },
        { 4, 0 }, { 4, 2 }, { 4, UINT8_MAX },
        { 6, 2 }, { 6, UINT8_MAX }, { 7, 8 }, { 7, 39 }, { 7, UINT8_MAX },
        { 8, 1 }, { 9, 1 }, { 10, 1 }, { 11, 1 },
    };
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        uint8_t damaged[DINO_SAVE_V1_SIZE];
        memcpy(damaged, original, sizeof(damaged));
        damaged[invalid[i].offset] = invalid[i].value;
        update_crc(damaged, sizeof(damaged));
        CHECK(rejected_without_mutation(damaged, sizeof(damaged)));
    }
    return true;
}

static bool test_playback_and_footprint_pages_do_not_change_record(void) {
    dino_model_t model = { .page = DINO_PAGE_MOTION, .dinosaur = 39,
        .found = DINO_FOUND_MASK };
    uint8_t original[DINO_SAVE_SIZE], after[DINO_SAVE_SIZE];
    dino_model_save(&model, original);
    CHECK(dino_model_tick(&model, 777));
    dino_effect_t effect = dino_model_handle(&model, DINO_INPUT_OK, 0);
    CHECK(model.paused && !effect.persist);
    dino_model_save(&model, after);
    CHECK(memcmp(original, after, sizeof(after)) == 0);
    (void)dino_model_handle(&model, DINO_INPUT_EXIT, 0);
    model.camp = 0;
    (void)dino_model_handle(&model, DINO_INPUT_OK, 0);
    effect = dino_model_handle(&model, DINO_INPUT_UP, 0);
    CHECK(model.footprints_page == 4 && !effect.persist);
    dino_model_save(&model, after);
    CHECK(memcmp(original, after, sizeof(after)) == 0);
    return true;
}

typedef struct {
    const char *name;
    bool (*run)(void);
} test_case_t;

int main(void) {
    const test_case_t tests[] = {
        { "intro and card navigation", test_intro_and_card_navigation },
        { "story navigation and replay", test_story_navigation_and_replay },
        { "all dinosaurs can collect footprints", test_all_dinosaurs_can_collect_footprints },
        { "wrong answers offer retry", test_wrong_answers_offer_retry_without_penalty },
        { "repeat collection avoids storage writes", test_repeat_collection_avoids_storage_writes },
        { "quiz choices wrap", test_quiz_choices_wrap_both_directions },
        { "camp preferences and footprints", test_camp_preferences_and_footprint_return },
        { "long return preserves collection", test_long_return_preserves_dinosaur_and_footprints },
        { "mute suppresses narration", test_muted_actions_suppress_every_narration },
        { "ignored inputs and invalid arguments", test_ignored_inputs_and_invalid_arguments },
        { "canonical save format", test_save_has_canonical_format },
        { "representative persisted states roundtrip", test_representative_persisted_states_roundtrip },
        { "single bit corruption is rejected", test_any_single_bit_corruption_is_rejected },
        { "invalid fields with valid CRC", test_invalid_fields_rejected_even_with_valid_crc },
        { "exact record length", test_load_requires_exact_record_length },
        { "invalid runtime state", test_invalid_runtime_state_is_ignored },
        { "large event sequences preserve invariants", test_large_event_sequences_preserve_invariants },
        { "footprint pages wrap and preserve high bits", test_footprint_pages_wrap_and_preserve_high_bits },
        { "camp motion entry starts playing", test_camp_motion_entry_starts_playing_silently },
        { "motion browses all dinosaurs", test_motion_browses_all_dinosaurs_preserving_pause },
        { "motion pause excludes elapsed time", test_motion_pause_excludes_elapsed_time },
        { "motion replay clears frame and remainder", test_motion_replay_clears_frame_and_remainder },
        { "motion exit returns to camp", test_motion_exit_returns_to_camp_selection },
        { "motion tick boundaries and large intervals", test_motion_tick_boundaries_and_large_intervals },
        { "motion ticks independent per instance", test_motion_ticks_are_independent_per_instance },
        { "motion tick ignores invalid or inactive pages", test_motion_tick_is_noop_outside_motion_or_invalid },
        { "V1 migration preserves all original states", test_v1_migration_preserves_all_original_states },
        { "V1 corruption and invalid cursor", test_v1_rejects_every_bit_corruption_and_invalid_cursor },
        { "V1/V2 version and length match", test_v1_and_v2_versions_require_matching_lengths },
        { "V1 invalid structure with valid CRC", test_v1_invalid_structure_with_valid_crc },
        { "transient playback/pages do not change record", test_playback_and_footprint_pages_do_not_change_record },
    };
    unsigned failures = 0;
    const unsigned count = sizeof(tests) / sizeof(tests[0]);
    for (unsigned i = 0; i < count; ++i) {
        if (!tests[i].run()) {
            fprintf(stderr, "FAIL: %s\n", tests[i].name);
            ++failures;
        }
    }
    printf("Dino model: %u/%u tests passed\n", count - failures, count);
    return failures ? 1 : 0;
}
