#include "dino_ui.h"
#include "dino_catalog.h"
#include "dino_animation.h"
#include "lvgl.h"
#include "src/misc/cache/instance/lv_image_cache.h"
#include <stdio.h>

LV_FONT_DECLARE(dino_font_14);
LV_FONT_DECLARE(dino_font_18);
LV_FONT_DECLARE(dino_font_24);
#define PAPER 0xF7F4E9
#define INK 0x244D3C
#define LEAF 0xCDE0B8
#define SOFT 0xE7EBDD
#define MUTED 0x637867
static lv_obj_t *s_screen, *s_body, *s_header, *s_battery, *s_footer;
static lv_obj_t *s_picture;
static dino_page_t s_page;
static unsigned s_species, s_frame;
static uint16_t s_pixels[DINO_ANIMATION_PIXELS];
static const lv_image_dsc_t s_image = {
    .header = { .magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_RGB565,
                .w = DINO_ANIMATION_WIDTH, .h = DINO_ANIMATION_HEIGHT,
                .stride = DINO_ANIMATION_WIDTH * 2 },
    .data_size = sizeof(s_pixels), .data = (const uint8_t *)s_pixels
};

static lv_obj_t *box(lv_obj_t *parent, int x, int y, int w, int h,
                     unsigned color, int radius) {
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(o, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(o, radius, 0);
    return o;
}
static lv_obj_t *text(lv_obj_t *parent, int x, int y, int w, int h,
                      const char *value, int size, unsigned color, bool center) {
    lv_obj_t *o = lv_label_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_label_set_long_mode(o, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(o, size == 24 ? &dino_font_24 :
                              size == 18 ? &dino_font_18 : &dino_font_14, 0);
    lv_obj_set_style_text_color(o, lv_color_hex(color), 0);
    lv_obj_set_style_text_align(o, center ? LV_TEXT_ALIGN_CENTER : LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_style_text_line_space(o, 2, 0);
    lv_label_set_text(o, value);
    return o;
}
static void picture(unsigned dinosaur, unsigned frame) {
    if (!dino_animation_decode(dinosaur, frame, s_pixels, DINO_ANIMATION_PIXELS)) return;
    if (!s_picture) {
        s_picture = lv_image_create(s_body);
        lv_image_set_src(s_picture, &s_image);
        lv_image_set_pivot(s_picture, 0, 0);
        lv_image_set_scale(s_picture, 384);
        lv_obj_set_pos(s_picture, 12, 0);
    } else {
        lv_image_cache_drop(&s_image);
        lv_obj_invalidate(s_picture);
    }
    s_frame = frame;
}
static unsigned count_found(uint64_t found) {
    unsigned count = 0;
    for (unsigned i = 0; i < DINO_COUNT; ++i) count += (found >> i) & 1u;
    return count;
}
static void footprint(int x, int y) {
    box(s_body, x + 11, y + 27, 39, 46, INK, 18);
    box(s_body, x, y + 8, 18, 25, INK, 9);
    box(s_body, x + 23, y, 18, 27, INK, 9);
    box(s_body, x + 47, y + 8, 18, 25, INK, 9);
}
void dino_ui_init(void) {
    s_screen = box(NULL, 0, 0, 240, 320, PAPER, 0);
    s_body = box(s_screen, 0, 42, 240, 235, PAPER, 0);
    s_header = text(s_screen, 27, 13, 144, 27, "小小恐龙护照", 14, INK, false);
    s_battery = text(s_screen, 178, 13, 40, 27, "--%", 14, MUTED, true);
    s_footer = text(s_screen, 27, 280, 186, 40, "", 14, MUTED, true);
    lv_screen_load(s_screen);
}
void dino_ui_render(const dino_model_t *m, int battery, int audio, int storage) {
    if (!s_screen || !m || m->dinosaur >= DINO_COUNT) return;
    const dino_entry_t *d = &dino_catalog[m->dinosaur];
    char buffer[80];
    const char *footer = "长按确定返回";
    const bool motion_update = s_picture && s_page == DINO_PAGE_MOTION &&
        m->page == DINO_PAGE_MOTION && s_species == m->dinosaur;
    if (!motion_update) {
        lv_obj_clean(s_body);
        s_picture = NULL;
    }
    if (battery < 0) lv_label_set_text(s_battery, "--%");
    else lv_label_set_text_fmt(s_battery, "%d%%", battery);
    lv_label_set_text(s_header, m->muted ? "恐龙护照 · 静音" : "小小恐龙护照");
    switch (m->page) {
    case DINO_PAGE_INTRO:
        picture(m->dinosaur, 0);
        text(s_body, 18, 136, 204, 37, "一起发现恐龙", 24, INK, true);
        text(s_body, 26, 177, 188, 58, "听一听，猜一猜\n留下你的发现足迹", 18, MUTED, true);
        footer = "按确定，开始探险";
        break;
    case DINO_PAGE_CARD:
        picture(m->dinosaur, 0);
        text(s_body, 17, 132, 206, 37, d->name, 24, INK, true);
        text(s_body, 18, 172, 204, 30, d->trait, 18, INK, true);
        snprintf(buffer, sizeof(buffer), "%s · %s", d->period, d->diet);
        text(s_body, 17, 207, 206, 27, buffer, 14, MUTED, true);
        snprintf(buffer, sizeof(buffer), "%u / %u%s", m->dinosaur + 1, DINO_COUNT,
                 (m->found & (UINT64_C(1) << m->dinosaur)) ? " · 已发现" : "");
        lv_label_set_text(s_header, buffer);
        footer = "上下翻阅 · 确定听知识\n长按确定，进入营地";
        break;
    case DINO_PAGE_STORY:
        text(s_body, 21, 9, 198, 37, d->name, 24, INK, false);
        if (m->story < 2) {
            snprintf(buffer, sizeof(buffer), "小知识 %u / 2", m->story + 1);
            text(s_body, 22, 53, 196, 27, buffer, 14, MUTED, false);
            text(s_body, 22, 87, 196, 147, d->facts[m->story], 18, INK, false);
            footer = "上下翻页 · 确定重听\n再下一页，来猜一猜";
        } else {
            footprint(86, 63);
            text(s_body, 20, 153, 200, 37, "来猜一猜", 24, INK, true);
            text(s_body, 21, 197, 198, 30, "答错也没关系，可以再试", 14, MUTED, true);
            footer = "确定开始 · 上下翻页\n长按确定返回";
        }
        break;
    case DINO_PAGE_QUIZ:
        text(s_body, 23, 9, 194, 27, "看仔细，再猜一猜", 14, MUTED, false);
        text(s_body, 22, 47, 196, 72, d->question, 18, INK, false);
        for (unsigned i = 0; i < 2; ++i) {
            lv_obj_t *row = box(s_body, 18, 126 + (int)i * 51, 204, 43,
                                m->choice == i ? INK : SOFT, 12);
            text(row, 10, 6, 184, 31, d->options[i], 18,
                 m->choice == i ? PAPER : INK, true);
        }
        footer = "上下选择 · 确定回答\n长按确定返回";
        break;
    case DINO_PAGE_FEEDBACK:
        text(s_body, 18, 12, 204, 38, m->correct ? "你发现啦！" : "再观察一下", 24, INK, true);
        footprint(87, 65);
        text(s_body, 22, 158, 196, 77,
             m->correct ? "每次认真观察\n都会有新的发现。" : d->hint, 18, INK, true);
        footer = m->correct ? "按确定，留下一枚足迹" : "按确定，再猜一次";
        break;
    case DINO_PAGE_STAMP:
        footprint(86, 22);
        text(s_body, 18, 108, 204, 37, "发现足迹", 24, INK, true);
        snprintf(buffer, sizeof(buffer), "%s · 已发现", d->name);
        text(s_body, 18, 151, 204, 33, buffer, 18, INK, true);
        snprintf(buffer, sizeof(buffer), "你已发现 %u / %u 种恐龙", count_found(m->found), DINO_COUNT);
        text(s_body, 20, 196, 200, 30, buffer, 14, MUTED, true);
        footer = "按确定，发现下一只";
        break;
    case DINO_PAGE_CAMP: {
        text(s_body, 20, 7, 200, 38, "探险营地", 24, INK, true);
        const char *items[DINO_CAMP_COUNT] = { "我的足迹", m->muted ? "打开声音" : "关闭声音", "恐龙动起来", "继续发现" };
        for (unsigned i = 0; i < DINO_CAMP_COUNT; ++i) {
            lv_obj_t *row = box(s_body, 18, 48 + (int)i * 46, 204, 40,
                                m->camp == i ? INK : SOFT, 12);
            text(row, 8, 4, 188, 32, items[i], 18, m->camp == i ? PAPER : INK, true);
        }
        footer = "上下选择 · 确定进入\n长按确定返回";
        break;
    }
    case DINO_PAGE_FOOTPRINTS:
        snprintf(buffer, sizeof(buffer), "发现足迹 %u / %u", count_found(m->found), DINO_COUNT);
        text(s_body, 18, 4, 204, 37, buffer, 24, INK, true);
        for (unsigned cell = 0; cell < DINO_FOOTPRINTS_PER_PAGE; ++cell) {
            const unsigned i = m->footprints_page * DINO_FOOTPRINTS_PER_PAGE + cell;
            if (i >= DINO_COUNT) break;
            const bool found = (m->found & (UINT64_C(1) << i)) != 0;
            lv_obj_t *row = box(s_body, 18 + (int)(cell % 2) * 105,
                                52 + (int)(cell / 2) * 43, 99, 35, found ? LEAF : SOFT, 9);
            text(row, 3, 5, 93, 28, dino_catalog[i].name, 18, found ? INK : MUTED, true);
        }
        snprintf(buffer, sizeof(buffer), "足迹第 %u / %u 页", m->footprints_page + 1, DINO_FOOTPRINT_PAGE_COUNT);
        lv_label_set_text(s_header, buffer);
        footer = "上下翻页 · 绿色为已发现\n确定返回，继续发现";
        break;
    case DINO_PAGE_MOTION:
        if (!motion_update || s_frame != m->frame) picture(m->dinosaur, m->frame);
        if (!motion_update) {
            text(s_body, 17, 132, 206, 37, d->name, 24, INK, true);
            snprintf(buffer, sizeof(buffer), "%s · %s", d->period, d->diet);
            text(s_body, 17, 177, 206, 30, buffer, 14, MUTED, true);
            text(s_body, 17, 211, 206, 24, "行走示意 · AI插画", 14, MUTED, true);
        }
        snprintf(buffer, sizeof(buffer), "%u / %u · %s", m->dinosaur + 1, DINO_COUNT,
                 m->paused ? "已暂停" : "播放中");
        lv_label_set_text(s_header, buffer);
        footer = "上下切换 · 确定暂停\n长按确定重播 · 长按上返回";
        break;
    }
    /* Degradation must not hide the current page's controls. */
    if (storage < 0) lv_label_set_text(s_header, "足迹暂不保存");
    else if (audio < 0 && !m->muted) lv_label_set_text(s_header, "声音暂不可用");
    lv_label_set_text(s_footer, footer);
    s_page = m->page;
    s_species = m->dinosaur;
    lv_obj_update_layout(s_screen);
}
