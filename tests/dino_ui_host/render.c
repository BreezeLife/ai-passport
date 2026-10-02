/* Real LVGL host rendering. This is not a hardware display acceptance test. */
#include "dino_catalog.h"
#include "dino_animation.h"
#include "dino_ui.h"
#include "lvgl.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

LV_FONT_DECLARE(dino_font_14);
LV_FONT_DECLARE(dino_font_18);
LV_FONT_DECLARE(dino_font_24);

#define WIDTH 240
#define HEIGHT 320
#define GUARD UINT32_C(0xc0decafe)

static struct {
    uint32_t before;
    uint16_t pixels[WIDTH * HEIGHT];
    uint32_t after;
} frame = { .before = GUARD, .after = GUARD };
static struct {
    uint32_t before;
    uint16_t pixels[WIDTH * 20];
    uint32_t after;
} draw = { .before = GUARD, .after = GUARD };

static unsigned failures, labels, glyphs, pages, flushes, log_failures;
static unsigned motion_checks, motion_pixel_changes, continuous_frames;
static char current_case[120];
static const char *output_directory;

static void fail(const char *reason, const char *detail) {
    fprintf(stderr, "FAIL [%s]: %s: %s\n", current_case, reason, detail);
    ++failures;
}

static void log_callback(lv_log_level_t level, const char *message) {
    if (level <= LV_LOG_LEVEL_ERROR) {
        fprintf(stderr, "LVGL [%s]: %s", current_case, message);
        ++log_failures;
    }
}

static void flush(lv_display_t *display, const lv_area_t *area, uint8_t *pixels) {
    if (area->x1 < 0 || area->y1 < 0 || area->x2 >= WIDTH || area->y2 >= HEIGHT) {
        fail("flush outside framebuffer", "240 x 320");
    } else {
        const unsigned width = (unsigned)(area->x2 - area->x1 + 1);
        const unsigned stride = lv_draw_buf_width_to_stride(width, LV_COLOR_FORMAT_RGB565);
        for (int y = area->y1; y <= area->y2; ++y)
            memcpy(&frame.pixels[y * WIDTH + area->x1],
                   pixels + (unsigned)(y - area->y1) * stride, width * 2);
    }
    ++flushes;
    lv_display_flush_ready(display);
}

static uint32_t next_codepoint(const char **text) {
    const unsigned char *p = (const unsigned char *)*text;
    uint32_t value = *p++;
    unsigned remaining = 0;
    if (value >= 0xf0) { value &= 7; remaining = 3; }
    else if (value >= 0xe0) { value &= 15; remaining = 2; }
    else if (value >= 0xc0) { value &= 31; remaining = 1; }
    while (remaining--) {
        if ((*p & 0xc0) != 0x80) {
            fail("invalid UTF-8", *text);
            *text = (const char *)p;
            return 0;
        }
        value = (value << 6) | (*p++ & 0x3f);
    }
    *text = (const char *)p;
    return value;
}

static void check_label(lv_obj_t *object) {
    const char *text = lv_label_get_text(object);
    const lv_font_t *font = lv_obj_get_style_text_font(object, LV_PART_MAIN);
    ++labels;
    if (font != &dino_font_14 && font != &dino_font_18 && font != &dino_font_24)
        fail("unexpected widget font", text);
    const char *cursor = text;
    while (*cursor) {
        uint32_t codepoint = next_codepoint(&cursor);
        if (codepoint == '\n' || codepoint == '\r') continue;
        lv_font_glyph_dsc_t descriptor;
        if (!lv_font_get_glyph_dsc(font, &descriptor, codepoint, 0) || descriptor.is_placeholder) {
            char detail[160];
            snprintf(detail, sizeof(detail), "U+%04X, lineheight=%u, text=%s",
                     (unsigned)codepoint, font->line_height, text);
            fail("missing rendered glyph", detail);
        }
        ++glyphs;
    }
    lv_point_t needed;
    lv_text_get_size(&needed, text, font,
                     lv_obj_get_style_text_letter_space(object, LV_PART_MAIN),
                     lv_obj_get_style_text_line_space(object, LV_PART_MAIN),
                     lv_obj_get_content_width(object), LV_TEXT_FLAG_NONE);
    if (needed.y > lv_obj_get_content_height(object) ||
        needed.x > lv_obj_get_content_width(object)) {
        char detail[300];
        snprintf(detail, sizeof(detail), "font lineheight=%u needs=%ldx%ld available=%ldx%ld text=%s",
                 font->line_height, (long)needed.x, (long)needed.y,
                 (long)lv_obj_get_content_width(object),
                 (long)lv_obj_get_content_height(object), text);
        fail("clipped label", detail);
    }
}

static void check_tree(lv_obj_t *object) {
    lv_obj_t *parent = lv_obj_get_parent(object);
    if (parent) {
        lv_area_t bounds, container;
        lv_obj_get_coords(object, &bounds);
        lv_obj_get_content_coords(parent, &container);
        if (bounds.x1 < container.x1 || bounds.y1 < container.y1 ||
            bounds.x2 > container.x2 || bounds.y2 > container.y2) {
            char detail[160];
            snprintf(detail, sizeof(detail), "widget=(%ld,%ld)-(%ld,%ld) parent=(%ld,%ld)-(%ld,%ld)",
                     (long)bounds.x1, (long)bounds.y1, (long)bounds.x2, (long)bounds.y2,
                     (long)container.x1, (long)container.y1, (long)container.x2, (long)container.y2);
            fail("widget exceeds parent", detail);
        }
    }
    if (lv_obj_check_type(object, &lv_image_class)) {
        lv_point_t pivot;
        lv_image_get_pivot(object, &pivot);
        const int32_t width = lv_image_get_transformed_width(object);
        const int32_t height = lv_image_get_transformed_height(object);
        if (pivot.x != 0 || pivot.y != 0 || lv_image_get_rotation(object) != 0 ||
            lv_image_get_scale_x(object) != 384 || lv_image_get_scale_y(object) != 384 ||
            width != 216 || height != 132)
            fail("unexpected image transform", "144x88, pivot 0, scale 1.5, drawn 216x132");
        if (parent) {
            lv_area_t bounds, container;
            lv_obj_get_coords(object, &bounds);
            lv_obj_get_content_coords(parent, &container);
            if (bounds.x1 < container.x1 || bounds.y1 < container.y1 ||
                bounds.x1 + width - 1 > container.x2 || bounds.y1 + height - 1 > container.y2)
                fail("scaled image exceeds parent", "drawn bounds must fit the 235-pixel body");
        }
    }
    if (lv_obj_check_type(object, &lv_label_class)) check_label(object);
    for (uint32_t i = 0; i < lv_obj_get_child_count(object); ++i)
        check_tree(lv_obj_get_child(object, (int32_t)i));
}

static void save_frame(const char *name) {
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s.ppm", output_directory, name);
    FILE *file = fopen(path, "wb");
    if (!file) { fail("cannot write screenshot", path); return; }
    fprintf(file, "P6\n%d %d\n255\n", WIDTH, HEIGHT);
    for (unsigned i = 0; i < WIDTH * HEIGHT; ++i) {
        const uint16_t pixel = frame.pixels[i];
        const uint8_t rgb[3] = {
            (uint8_t)(((pixel >> 11) & 31) * 255 / 31),
            (uint8_t)(((pixel >> 5) & 63) * 255 / 63),
            (uint8_t)((pixel & 31) * 255 / 31),
        };
        if (fwrite(rgb, 1, 3, file) != 3) { fail("screenshot write failed", path); break; }
    }
    fclose(file);
}

static void render(lv_display_t *display, const dino_model_t *model,
                   int battery, int audio, int storage, const char *capture) {
    dino_ui_render(model, battery, audio, storage);
    lv_tick_inc(30);
    lv_timer_handler();
    lv_refr_now(display);
    check_tree(lv_screen_active());
    lv_obj_t *body = lv_obj_get_child(lv_screen_active(), 0);
    if (lv_obj_get_height(body) != 235 || lv_obj_get_y(body) != 42)
        fail("unexpected body geometry", "expected 240 x 235 at (0,42)");
    if (frame.before != GUARD || frame.after != GUARD ||
        draw.before != GUARD || draw.after != GUARD)
        fail("frame or draw buffer guard corrupted", "out-of-bounds write");
    if (lv_mem_test() != LV_RESULT_OK) fail("LVGL heap corrupt", "48 KiB pool");
    if (capture) save_frame(capture);
    ++pages;
}

static unsigned variants(dino_page_t page) {
    if (page == DINO_PAGE_STORY) return 3;
    if (page == DINO_PAGE_CAMP) return DINO_CAMP_COUNT;
    if (page == DINO_PAGE_FOOTPRINTS) return DINO_FOOTPRINT_PAGE_COUNT;
    if (page == DINO_PAGE_MOTION) return DINO_FRAME_COUNT * 2;
    if (page == DINO_PAGE_QUIZ || page == DINO_PAGE_FEEDBACK) return 2;
    return 1;
}

/* Only the displayed picture area: header/status changes cannot disguise a
   stale image. Hash pixels after real LVGL refresh/flush, not decoder memory. */
static uint64_t screen_picture_hash(lv_obj_t *image) {
    lv_area_t bounds;
    lv_obj_get_coords(image, &bounds);
    const int32_t width = lv_image_get_transformed_width(image);
    const int32_t height = lv_image_get_transformed_height(image);
    if (bounds.x1 < 0 || bounds.y1 < 0 || bounds.x1 + width > WIDTH ||
        bounds.y1 + height > HEIGHT) {
        fail("cannot read transformed screenshot", "image outside framebuffer");
        return 0;
    }
    uint64_t hash = UINT64_C(14695981039346656037);
    for (int32_t y = bounds.y1; y < bounds.y1 + height; ++y) {
        for (int32_t x = bounds.x1; x < bounds.x1 + width; ++x) {
            const uint16_t pixel = frame.pixels[y * WIDTH + x];
            hash = (hash ^ (pixel & 255)) * UINT64_C(1099511628211);
            hash = (hash ^ (pixel >> 8)) * UINT64_C(1099511628211);
        }
    }
    return hash;
}

static void check_motion_screen(lv_display_t *display, unsigned dinosaur,
                                uint64_t hashes[DINO_FRAME_COUNT]) {
    dino_model_t model = { .page = DINO_PAGE_MOTION, .dinosaur = (uint8_t)dinosaur };
    snprintf(current_case, sizeof(current_case), "motion screen species=%u frame=0", dinosaur);
    render(display, &model, 87, 1, 1, NULL);
    lv_obj_t *body = lv_obj_get_child(lv_screen_active(), 0);
    lv_obj_t *image = lv_obj_get_child(body, 0);
    if (!image || !lv_obj_check_type(image, &lv_image_class)) {
        fail("motion image missing", "a real decoded animation must be visible");
        return;
    }
    const uint32_t children = lv_obj_get_child_count(body);
    lv_obj_t *widgets[4];
    if (children != 4) { fail("unexpected motion widgets", "one image and three labels"); return; }
    for (unsigned i = 0; i < children; ++i) widgets[i] = lv_obj_get_child(body, (int32_t)i);
    const lv_image_dsc_t *source = lv_image_get_src(image);
    if (!source || source->header.cf != LV_COLOR_FORMAT_RGB565 ||
        source->header.w != DINO_ANIMATION_WIDTH || source->header.h != DINO_ANIMATION_HEIGHT ||
        source->header.stride != DINO_ANIMATION_WIDTH * 2 || source->data_size != DINO_ANIMATION_PIXELS * 2) {
        fail("invalid motion source descriptor", "144x88 RGB565, stride 288");
        return;
    }
    uint16_t previous[DINO_ANIMATION_PIXELS];
    memcpy(previous, source->data, sizeof(previous));
    hashes[0] = screen_picture_hash(image);
    for (unsigned number = 1; number < DINO_FRAME_COUNT; ++number) {
        model.frame = (uint8_t)number;
        snprintf(current_case, sizeof(current_case), "motion screen species=%u frame=%u", dinosaur, number);
        render(display, &model, 87, 1, 1, NULL);
        if (children != lv_obj_get_child_count(body)) fail("motion rebuilds widgets", "child count changed");
        for (unsigned i = 0; i < children; ++i)
            if (widgets[i] != lv_obj_get_child(body, (int32_t)i))
                fail("motion rebuilds widgets", "every image and label must be retained");
        if (source != lv_image_get_src(image)) fail("motion replaces descriptor", "one descriptor must be reused");
        if (!memcmp(previous, source->data, sizeof(previous)))
            fail("motion does not change decoded pixels", "consecutive original frames must differ");
        hashes[number] = screen_picture_hash(image);
        for (unsigned earlier = 0; earlier < number; ++earlier)
            if (hashes[earlier] == hashes[number])
                fail("motion screen is stale", "eight distinct frames must reach the real framebuffer");
        memcpy(previous, source->data, sizeof(previous));
        ++motion_pixel_changes;
    }
    model.paused = true;
    render(display, &model, 87, 1, 1, NULL);
    if (memcmp(previous, source->data, sizeof(previous)) ||
        screen_picture_hash(image) != hashes[DINO_FRAME_COUNT - 1])
        fail("pause redraw changes frame", "same frame must retain exact displayed pixels");
    dino_model_handle(&model, DINO_INPUT_BACK, 0);
    render(display, &model, 87, 1, 1, NULL);
    if (model.frame != 0 || model.paused || screen_picture_hash(image) != hashes[0])
        fail("replay does not show frame zero", "long OK must restart playing the first real frame");
    ++motion_checks;
}

int main(int argc, char **argv) {
    if (argc != 2) { fprintf(stderr, "Usage: dino_ui_host <existing output directory>\n"); return 2; }
    output_directory = argv[1];
    printf("LVGL %d.%d.%d; font line heights: 14=%u 18=%u 24=%u; pool=%u bytes\n",
           LVGL_VERSION_MAJOR, LVGL_VERSION_MINOR, LVGL_VERSION_PATCH,
           dino_font_14.line_height, dino_font_18.line_height,
           dino_font_24.line_height, (unsigned)LV_MEM_SIZE);
    if (LVGL_VERSION_MAJOR != 9 || LVGL_VERSION_MINOR != 5 || LVGL_VERSION_PATCH != 0) {
        fprintf(stderr, "Expected exactly LVGL 9.5.0\n"); return 2;
    }
    lv_init();
    lv_log_register_print_cb(log_callback);
    lv_display_t *display = lv_display_create(WIDTH, HEIGHT);
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(display, draw.pixels, NULL, sizeof(draw.pixels), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display, flush);
    dino_ui_init();
    /* Prove the glyph checker can distinguish absent glyphs from placeholders. */
    lv_font_glyph_dsc_t absent;
    if (lv_font_get_glyph_dsc(&dino_font_18, &absent, 0x9f98, 0) || !absent.is_placeholder)
        fail("missing glyph negative control failed", "U+9F98 should be absent");

    for (unsigned dinosaur = 0; dinosaur < DINO_COUNT; ++dinosaur) {
        for (unsigned muted = 0; muted < 2; ++muted) {
            for (unsigned status = 0; status < 4; ++status) {
                for (dino_page_t page = DINO_PAGE_INTRO; page <= DINO_PAGE_MOTION; ++page) {
                    for (unsigned variant = 0; variant < variants(page); ++variant) {
                        dino_model_t model = { .page = page, .dinosaur = (uint8_t)dinosaur,
                            .story = page == DINO_PAGE_STORY ? (uint8_t)variant : 0,
                            .choice = page == DINO_PAGE_QUIZ ? (uint8_t)variant : 0,
                            .camp = page == DINO_PAGE_CAMP ? (uint8_t)variant : 0,
                            .found = UINT64_C(1) << dinosaur, .muted = muted != 0,
                            .footprints_page = page == DINO_PAGE_FOOTPRINTS ? (uint8_t)variant : 0,
                            .frame = page == DINO_PAGE_MOTION ? (uint8_t)(variant % DINO_FRAME_COUNT) : 0,
                            .paused = page == DINO_PAGE_MOTION && variant >= DINO_FRAME_COUNT,
                            .correct = page == DINO_PAGE_FEEDBACK && variant != 0 };
                        snprintf(current_case, sizeof(current_case), "dino=%u page=%u variant=%u muted=%u status=%u",
                                 dinosaur, (unsigned)page, variant, muted, status);
                        char capture[100];
                        const bool selected = muted == 0 && status == 0 &&
                            (page == DINO_PAGE_CARD || (page == DINO_PAGE_STORY && variant < 2) ||
                             (page == DINO_PAGE_FEEDBACK && variant == 0) ||
                             (page == DINO_PAGE_MOTION && variant < DINO_FRAME_COUNT) || dinosaur == 0);
                        snprintf(capture, sizeof(capture), "%02u-dino-%u-page-%u-variant-%u",
                                 (unsigned)page, dinosaur + 1, (unsigned)page, variant);
                        render(display, &model, status == 0 ? 87 : status == 1 ? -1 : status == 2 ? 0 : 100,
                               status & 1 ? -1 : 1, status & 2 ? -1 : 1, selected ? capture : NULL);
                    }
                }
            }
        }
    }
    for (unsigned found = 0; found < 3; ++found) {
        dino_model_t model = { .page = DINO_PAGE_FOOTPRINTS,
            .found = found == 0 ? 0 : found == 1 ? UINT64_C(0x80000000a5) : DINO_FOUND_MASK };
        char capture[80];
        snprintf(capture, sizeof(capture), "08-footprints-%s", found == 0 ? "empty" : found == 1 ? "partial" : "full");
        snprintf(current_case, sizeof(current_case), "%s", capture);
        render(display, &model, 87, 1, 1, capture);
    }

    uint64_t screen_hashes[DINO_COUNT][DINO_FRAME_COUNT] = {{0}};
    for (unsigned dinosaur = 0; dinosaur < DINO_COUNT; ++dinosaur)
        check_motion_screen(display, dinosaur, screen_hashes[dinosaur]);

    /* One uninterrupted 8000-frame motion session. No page/selection change
       may conceal frame-path allocations by rebuilding the body. */
    dino_model_t playing = { .page = DINO_PAGE_MOTION, .dinosaur = DINO_COUNT - 1 };
    snprintf(current_case, sizeof(current_case), "continuous motion baseline");
    render(display, &playing, 87, 1, 1, NULL);
    lv_obj_t *playing_body = lv_obj_get_child(lv_screen_active(), 0);
    lv_obj_t *playing_widgets[4];
    if (lv_obj_get_child_count(playing_body) != 4)
        fail("continuous motion missing widgets", "one image and three labels");
    for (unsigned i = 0; i < 4; ++i)
        playing_widgets[i] = lv_obj_get_child(playing_body, (int32_t)i);
    lv_mem_monitor_t playing_initial, playing_final;
    lv_mem_monitor(&playing_initial);
    for (unsigned number = 0; number < 8000; ++number) {
        snprintf(current_case, sizeof(current_case), "continuous motion frame=%u", number);
        if (!dino_model_tick(&playing, DINO_FRAME_MS))
            fail("animation tick did not advance", "125 ms must change the active frame");
        render(display, &playing, 87, 1, 1, NULL);
        if (lv_obj_get_child_count(playing_body) != 4)
            fail("continuous motion rebuilds body", "child count changed");
        for (unsigned i = 0; i < 4; ++i)
            if (playing_widgets[i] != lv_obj_get_child(playing_body, (int32_t)i))
                fail("continuous motion rebuilds widgets", "8000 frames must retain all widgets");
        if (screen_picture_hash(playing_widgets[0]) != screen_hashes[DINO_COUNT - 1][playing.frame])
            fail("continuous motion stale framebuffer", "visible frame must match previously rendered original");
        ++continuous_frames;
    }
    lv_mem_monitor(&playing_final);
    if (playing_final.free_size != playing_initial.free_size ||
        playing_final.free_biggest_size < playing_initial.free_biggest_size)
        fail("continuous motion heap growth/fragmentation", "8000 frames return to the same exact frame");
    for (unsigned status = 0; status < 3; ++status) {
        dino_model_t model = { .page = DINO_PAGE_CAMP, .muted = status == 0 };
        const char *capture = status == 0 ? "09-muted" : status == 1 ? "09-audio-unavailable" : "09-storage-unavailable";
        snprintf(current_case, sizeof(current_case), "%s", capture);
        render(display, &model, -1, status == 1 ? -1 : 1, status == 2 ? -1 : 1, capture);
    }

    dino_model_t baseline;
    dino_model_init(&baseline);
    render(display, &baseline, 87, 1, 1, NULL);
    lv_mem_monitor_t initial, final;
    lv_mem_monitor(&initial);
    const unsigned validated_cases = pages;
    for (unsigned round = 0; round < 1000; ++round) {
        for (dino_page_t page = DINO_PAGE_INTRO; page <= DINO_PAGE_MOTION; ++page) {
            dino_model_t model = { .page = page, .dinosaur = (uint8_t)(round % DINO_COUNT),
                .story = (uint8_t)(round % 3), .choice = (uint8_t)(round % 2),
                .camp = (uint8_t)(round % DINO_CAMP_COUNT), .found = round & DINO_FOUND_MASK,
                .footprints_page = (uint8_t)(round % DINO_FOOTPRINT_PAGE_COUNT),
                .frame = (uint8_t)(round % DINO_FRAME_COUNT), .paused = (round & 1) != 0,
                .muted = (round & 1) != 0, .correct = (round & 2) != 0 };
            snprintf(current_case, sizeof(current_case), "stress round=%u page=%u", round, (unsigned)page);
            render(display, &model, (int)(round % 102) - 1,
                   round % 7 ? 1 : -1, round % 11 ? 1 : -1, NULL);
        }
    }
    snprintf(current_case, sizeof(current_case), "post-stress baseline");
    render(display, &baseline, 87, 1, 1, NULL);
    lv_mem_monitor(&final);
    if (final.free_size < initial.free_size)
        fail("net heap growth after 1000 rounds", "same baseline page must not retain more memory");
    printf("UI cases=%u; motion species checks=%u; distinct screen transitions=%u; continuous frames=%u; stress renders=9000; labels=%u; glyphs=%u; flushes=%u\n",
           validated_cases, motion_checks, motion_pixel_changes, continuous_frames, labels, glyphs, flushes);
    printf("Continuous motion heap baseline_free=%zu final_free=%zu baseline_largest=%zu final_largest=%zu\n",
           playing_initial.free_size, playing_final.free_size,
           playing_initial.free_biggest_size, playing_final.free_biggest_size);
    printf("LVGL pool total=%zu peak_used=%zu baseline_free=%zu final_free=%zu largest_free=%zu\n",
           final.total_size, final.max_used, initial.free_size, final.free_size, final.free_biggest_size);
    printf("UI checks: %s (layout/glyph failures=%u, LVGL warnings/errors=%u)\n",
           failures || log_failures ? "FAIL" : "PASS", failures, log_failures);
    lv_deinit();
    return failures || log_failures ? 1 : 0;
}
