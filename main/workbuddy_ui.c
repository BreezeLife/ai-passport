#include "workbuddy_ui.h"

#include "fonts/lv_font_noto_sans_sc_14.h"
#include "lvgl.h"

#include <stdio.h>

#define WB_COLOR_BG 0xF5F7F4
#define WB_COLOR_SURFACE 0xFFFFFF
#define WB_COLOR_INK 0x18211D
#define WB_COLOR_MUTED 0x66736C
#define WB_COLOR_LINE 0xD9E1DC
#define WB_COLOR_ACCENT 0x15A05C
#define WB_COLOR_ACCENT_SOFT 0xE3F5EA
#define WB_COLOR_WARNING 0xD88913
#define WB_COLOR_DANGER 0xD94343
#define WB_COLOR_DARK 0x102A20

#define WB_STATUS_HEIGHT 24
#define WB_CONTENT_Y 24
#define WB_CONTENT_HEIGHT 248
#define WB_FOOTER_Y 272
#define WB_FOOTER_HEIGHT 48

static lv_obj_t *s_screen;
static lv_obj_t *s_status;
static lv_obj_t *s_content;
static lv_obj_t *s_footer;

static lv_obj_t *make_box(lv_obj_t *parent, int x, int y, int width, int height,
                          uint32_t color, int radius)
{
    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(box, x, y);
    lv_obj_set_size(box, width, height);
    lv_obj_set_style_pad_all(box, 0, 0);
    lv_obj_set_style_border_width(box, 0, 0);
    lv_obj_set_style_radius(box, radius, 0);
    lv_obj_set_style_bg_color(box, lv_color_hex(color), 0);
    return box;
}

static lv_obj_t *make_label(lv_obj_t *parent, const char *text, int x, int y,
                            int width, uint32_t color, const lv_font_t *font)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text != NULL ? text : "");
    lv_obj_set_pos(label, x, y);
    lv_obj_set_width(label, width);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_line_space(label, 3, 0);
    return label;
}

static void set_one_line(lv_obj_t *label)
{
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    lv_obj_set_height(label, 20);
}

static void draw_header(const wb_model_t *model, const wb_ui_runtime_t *runtime)
{
    char right[64];
    const char *wifi = runtime->wifi_label != NULL ? runtime->wifi_label : "OFFLINE";

    lv_obj_clean(s_status);
    make_label(s_status, "WorkBuddy", 8, 1, 78, WB_COLOR_SURFACE,
               &lv_font_montserrat_14);
    if (runtime->battery_percent >= 0) {
        (void)snprintf(right, sizeof(right), "%s  %d%%", wifi,
                       runtime->battery_percent);
    } else {
        (void)snprintf(right, sizeof(right), "%s", wifi);
    }
    lv_obj_t *label = make_label(s_status, right, 136, 2, 96, WB_COLOR_SURFACE,
                                 &lv_font_montserrat_14);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_RIGHT, 0);

    if (runtime->demo_mode || model->stale) {
        lv_obj_t *pill = make_box(s_status, runtime->demo_mode ? 87 : 90, 3,
                                  runtime->demo_mode ? 45 : 39, 18,
                                  runtime->demo_mode ? WB_COLOR_WARNING : WB_COLOR_DANGER,
                                  9);
        lv_obj_t *pill_text = make_label(pill, runtime->demo_mode ? "DEMO" : "STALE",
                                         3, 1, runtime->demo_mode ? 39 : 33,
                                         WB_COLOR_SURFACE, &lv_font_montserrat_14);
        lv_obj_set_style_text_align(pill_text, LV_TEXT_ALIGN_CENTER, 0);
    }
}

static void draw_footer(const char *text)
{
    lv_obj_clean(s_footer);
    lv_obj_t *label = make_label(s_footer, text, 8, 8, 224, WB_COLOR_SURFACE,
                                 &lv_font_noto_sans_sc_14);
    lv_obj_set_height(label, 32);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(label);
}

static void draw_section_title(const char *title, const char *counter)
{
    make_label(s_content, title, 10, 7, 150, WB_COLOR_INK,
               &lv_font_noto_sans_sc_14);
    if (counter != NULL) {
        lv_obj_t *label = make_label(s_content, counter, 164, 7, 66, WB_COLOR_MUTED,
                                     &lv_font_montserrat_14);
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_RIGHT, 0);
    }
}

static void draw_empty(const char *title, const char *body)
{
    draw_section_title(title, NULL);
    lv_obj_t *panel = make_box(s_content, 10, 52, 220, 132,
                               WB_COLOR_SURFACE, 14);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_border_color(panel, lv_color_hex(WB_COLOR_LINE), 0);
    lv_obj_t *label = make_label(panel, body, 16, 42, 188, WB_COLOR_MUTED,
                                 &lv_font_noto_sans_sc_14);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
}

static void draw_row(const char *title, const char *subtitle, int row,
                     bool selected, bool emphasized)
{
    int y = 34 + row * 52;
    lv_obj_t *panel = make_box(s_content, 8, y, 224, 46,
                               selected ? WB_COLOR_ACCENT_SOFT : WB_COLOR_SURFACE, 10);
    lv_obj_set_style_border_width(panel, selected ? 2 : 1, 0);
    lv_obj_set_style_border_color(panel,
        lv_color_hex(selected ? WB_COLOR_ACCENT : WB_COLOR_LINE), 0);
    if (emphasized) {
        make_box(panel, 7, 8, 5, 30, WB_COLOR_ACCENT, 3);
    }
    lv_obj_t *headline = make_label(panel, title, emphasized ? 18 : 12, 3,
                                    emphasized ? 194 : 200, WB_COLOR_INK,
                                    &lv_font_noto_sans_sc_14);
    set_one_line(headline);
    lv_obj_t *detail = make_label(panel, subtitle, emphasized ? 18 : 12, 24,
                                  emphasized ? 194 : 200, WB_COLOR_MUTED,
                                  &lv_font_noto_sans_sc_14);
    set_one_line(detail);
}

static size_t visible_start(size_t focus, size_t count)
{
    if (count <= 4U || focus < 2U) {
        return 0U;
    }
    size_t start = focus - 1U;
    return start + 4U <= count ? start : count - 4U;
}

static const char *role_name(wb_message_role_t role)
{
    switch (role) {
    case WB_MESSAGE_ROLE_ASSISTANT: return "WorkBuddy";
    case WB_MESSAGE_ROLE_USER: return "我";
    case WB_MESSAGE_ROLE_SYSTEM: return "系统";
    default: return "消息";
    }
}

static const char *task_name(wb_task_status_t status)
{
    switch (status) {
    case WB_TASK_STATUS_QUEUED: return "排队中";
    case WB_TASK_STATUS_RUNNING: return "进行中";
    case WB_TASK_STATUS_NEEDS_INPUT: return "等你确认";
    case WB_TASK_STATUS_COMPLETED: return "已完成";
    case WB_TASK_STATUS_FAILED: return "失败";
    default: return "未知状态";
    }
}

static const char *output_name(wb_output_kind_t kind)
{
    switch (kind) {
    case WB_OUTPUT_KIND_PLAN: return "计划";
    case WB_OUTPUT_KIND_CHECKLIST: return "清单";
    case WB_OUTPUT_KIND_OVERVIEW: return "总结";
    case WB_OUTPUT_KIND_IMAGE: return "图片";
    case WB_OUTPUT_KIND_DOCUMENT: return "文档";
    default: return "产出";
    }
}

static void draw_privacy(const wb_model_t *model)
{
    char counts[96];
    lv_obj_t *mark = make_box(s_content, 78, 35, 84, 84, WB_COLOR_DARK, 22);
    lv_obj_t *initials = make_label(mark, "WB", 0, 20, 84, WB_COLOR_SURFACE,
                                    &lv_font_montserrat_20);
    lv_obj_set_style_text_align(initials, LV_TEXT_ALIGN_CENTER, 0);

    (void)snprintf(counts, sizeof(counts), "消息 %u  ·  任务 %u",
                   model->snapshot.unread_count,
                   model->snapshot.active_task_count);
    lv_obj_t *label = make_label(s_content, counts, 15, 139, 210, WB_COLOR_INK,
                                 &lv_font_noto_sans_sc_14);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    make_label(s_content,
               model->snapshot.assistant_available ? "WorkBuddy 已连接" : "WorkBuddy 暂未连接",
               15, 172, 210, WB_COLOR_MUTED, &lv_font_noto_sans_sc_14);
    label = lv_obj_get_child(s_content, -1);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    make_label(s_content, "内容已隐藏", 15, 205, 210, WB_COLOR_MUTED,
               &lv_font_noto_sans_sc_14);
    label = lv_obj_get_child(s_content, -1);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    draw_footer("确定：查看消息    长按：新任务");
}

static void draw_inbox(const wb_model_t *model)
{
    char counter[24];
    (void)snprintf(counter, sizeof(counter), "%u/%u",
                   model->snapshot.message_count == 0U ? 0U :
                       (unsigned)model->inbox_focus + 1U,
                   (unsigned)model->snapshot.message_count);
    if (model->snapshot.message_count == 0U) {
        draw_empty("消息", "还没有 WorkBuddy 消息");
    } else {
        draw_section_title("消息", counter);
        size_t start = visible_start(model->inbox_focus, model->snapshot.message_count);
        size_t end = start + 4U < model->snapshot.message_count
            ? start + 4U : model->snapshot.message_count;
        for (size_t index = start; index < end; ++index) {
            const wb_message_t *message = &model->snapshot.messages[index];
            char title[WB_TITLE_CAP + 24U];
            (void)snprintf(title, sizeof(title), "%s%s  %s",
                           message->unread ? "● " : "",
                           role_name(message->role), message->title);
            draw_row(title, message->preview, (int)(index - start),
                     index == model->inbox_focus, message->unread);
        }
    }
    draw_footer("上下：选择    确定：语音回复    长按：菜单");
}

static void draw_tasks(const wb_model_t *model)
{
    char counter[24];
    (void)snprintf(counter, sizeof(counter), "%u/%u",
                   model->snapshot.task_count == 0U ? 0U :
                       (unsigned)model->task_focus + 1U,
                   (unsigned)model->snapshot.task_count);
    if (model->snapshot.task_count == 0U) {
        draw_empty("任务", "还没有云端任务\n长按打开菜单创建");
    } else {
        draw_section_title("任务", counter);
        size_t start = visible_start(model->task_focus, model->snapshot.task_count);
        size_t end = start + 4U < model->snapshot.task_count
            ? start + 4U : model->snapshot.task_count;
        for (size_t index = start; index < end; ++index) {
            const wb_task_t *task = &model->snapshot.tasks[index];
            char subtitle[WB_PREVIEW_CAP + 32U];
            (void)snprintf(subtitle, sizeof(subtitle), "%s · %s",
                           task_name(task->status), task->preview);
            draw_row(task->title, subtitle, (int)(index - start),
                     index == model->task_focus,
                     task->status == WB_TASK_STATUS_NEEDS_INPUT);
        }
    }
    draw_footer("上下：选择    确定：详情    长按：菜单");
}

static void draw_outputs(const wb_model_t *model)
{
    char counter[24];
    (void)snprintf(counter, sizeof(counter), "%u/%u",
                   model->snapshot.output_count == 0U ? 0U :
                       (unsigned)model->output_focus + 1U,
                   (unsigned)model->snapshot.output_count);
    if (model->snapshot.output_count == 0U) {
        draw_empty("产出", "任务产出会显示在这里");
    } else {
        draw_section_title("产出", counter);
        size_t start = visible_start(model->output_focus, model->snapshot.output_count);
        size_t end = start + 4U < model->snapshot.output_count
            ? start + 4U : model->snapshot.output_count;
        for (size_t index = start; index < end; ++index) {
            const wb_output_t *output = &model->snapshot.outputs[index];
            char subtitle[WB_PREVIEW_CAP + 24U];
            (void)snprintf(subtitle, sizeof(subtitle), "%s · %s",
                           output_name(output->kind), output->preview);
            draw_row(output->title, subtitle, (int)(index - start),
                     index == model->output_focus, false);
        }
    }
    draw_footer("上下：选择    确定：预览    长按：菜单");
}

static void draw_detail(const char *section, const char *title,
                        const char *status, const char *preview,
                        const char *footer)
{
    draw_section_title(section, status);
    lv_obj_t *panel = make_box(s_content, 8, 36, 224, 196,
                               WB_COLOR_SURFACE, 12);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_border_color(panel, lv_color_hex(WB_COLOR_LINE), 0);
    lv_obj_t *heading = make_label(panel, title, 12, 10, 200, WB_COLOR_INK,
                                   &lv_font_noto_sans_sc_14);
    lv_label_set_long_mode(heading, LV_LABEL_LONG_WRAP);
    lv_obj_set_height(heading, 46);
    lv_obj_t *body = make_label(panel, preview, 12, 65, 200, WB_COLOR_MUTED,
                                &lv_font_noto_sans_sc_14);
    lv_label_set_long_mode(body, LV_LABEL_LONG_WRAP);
    lv_obj_set_height(body, 115);
    draw_footer(footer);
}

static void draw_navigation(const wb_model_t *model)
{
    static const char *const ITEMS[WB_MENU_COUNT] = {
        "消息", "任务", "新任务", "产出", "立即同步", "连接状态"
    };
    draw_section_title("去哪儿", NULL);
    for (int index = 0; index < WB_MENU_COUNT; ++index) {
        int row = index / 2;
        int column = index % 2;
        bool selected = index == (int)model->menu_item;
        lv_obj_t *card = make_box(s_content, 8 + column * 114, 38 + row * 57,
                                  108, 48,
                                  selected ? WB_COLOR_ACCENT : WB_COLOR_SURFACE, 10);
        lv_obj_set_style_border_width(card, 1, 0);
        lv_obj_set_style_border_color(card,
            lv_color_hex(selected ? WB_COLOR_ACCENT : WB_COLOR_LINE), 0);
        lv_obj_t *label = make_label(card, ITEMS[index], 5, 13, 98,
                                     selected ? WB_COLOR_SURFACE : WB_COLOR_INK,
                                     &lv_font_noto_sans_sc_14);
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    }
    draw_footer("上下：选择    确定：进入    长按：关闭");
}

static const char *voice_context_name(wb_voice_context_t context)
{
    switch (context) {
    case WB_VOICE_REPLY: return "回复 WorkBuddy";
    case WB_VOICE_TASK_CREATE: return "创建新任务";
    case WB_VOICE_TASK_FOLLOWUP: return "继续追问任务";
    default: return "语音输入";
    }
}

static void draw_voice(const wb_model_t *model, const wb_ui_runtime_t *runtime)
{
    uint32_t elapsed = runtime->now_ms - model->recording_started_ms;
    unsigned tenths = elapsed / 100U;
    char timer[24];
    (void)snprintf(timer, sizeof(timer), "%u.%us / 5.0s",
                   tenths / 10U, tenths % 10U);
    draw_section_title(voice_context_name(model->voice_context), NULL);
    make_box(s_content, 101, 55, 38, 38, WB_COLOR_DANGER, 19);
    lv_obj_t *label = make_label(s_content, "正在聆听", 20, 111, 200,
                                 WB_COLOR_INK, &lv_font_noto_sans_sc_14);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    label = make_label(s_content, timer, 20, 148, 200, WB_COLOR_MUTED,
                       &lv_font_montserrat_20);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    make_label(s_content, "录音最长 5 秒，不会自动发送", 20, 194, 200,
               WB_COLOR_MUTED, &lv_font_noto_sans_sc_14);
    label = lv_obj_get_child(s_content, -1);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    draw_footer("确定：提前结束    长按：取消");
}

static void draw_centered_state(const char *title, const char *body,
                                uint32_t accent, const char *footer)
{
    draw_section_title(title, NULL);
    lv_obj_t *circle = make_box(s_content, 94, 61, 52, 52, accent, 26);
    lv_obj_t *dots = make_label(circle, "…", 0, 10, 52, WB_COLOR_SURFACE,
                                &lv_font_montserrat_20);
    lv_obj_set_style_text_align(dots, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_t *label = make_label(s_content, body, 20, 139, 200, WB_COLOR_INK,
                                 &lv_font_noto_sans_sc_14);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    draw_footer(footer);
}

static void draw_review(const wb_model_t *model)
{
    draw_section_title("发送前确认", NULL);
    lv_obj_t *panel = make_box(s_content, 8, 37, 224, 187,
                               WB_COLOR_ACCENT_SOFT, 12);
    lv_obj_set_style_border_width(panel, 2, 0);
    lv_obj_set_style_border_color(panel, lv_color_hex(WB_COLOR_ACCENT), 0);
    make_label(panel, voice_context_name(model->voice_context), 12, 10, 200,
               WB_COLOR_MUTED, &lv_font_noto_sans_sc_14);
    lv_obj_t *text = make_label(panel, model->transcript, 12, 42, 200,
                                WB_COLOR_INK, &lv_font_noto_sans_sc_14);
    lv_label_set_long_mode(text, LV_LABEL_LONG_WRAP);
    lv_obj_set_height(text, 128);
    draw_footer("确定：发送    下：重录    长按：取消");
}

static void draw_status(const wb_model_t *model, const wb_ui_runtime_t *runtime)
{
    char body[320];
    (void)snprintf(body, sizeof(body),
                   "模式：%s\nWi-Fi：%s\n网关：%s\n本地助理：%s\n麦克风：%s\n数据：%s",
                   runtime->demo_mode ? "演示" : "在线",
                   runtime->wifi_label != NULL ? runtime->wifi_label : "未知",
                   runtime->gateway_online ? "可用" : "不可用",
                   model->snapshot.assistant_available ? "在线" : "离线",
                   runtime->audio_ready ? "可用" : "不可用",
                   model->stale ? "缓存/过期" : "最新");
    draw_detail("连接状态", "WorkBuddy AI Passport", NULL, body,
                "长按：菜单");
}

bool wb_ui_init(void)
{
    if (s_screen != NULL) {
        return true;
    }
    s_screen = lv_obj_create(NULL);
    lv_obj_remove_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(s_screen, 0, 0);
    lv_obj_set_style_border_width(s_screen, 0, 0);
    lv_obj_set_style_bg_color(s_screen, lv_color_hex(WB_COLOR_BG), 0);

    s_status = make_box(s_screen, 0, 0, 240, WB_STATUS_HEIGHT,
                        WB_COLOR_DARK, 0);
    s_content = make_box(s_screen, 0, WB_CONTENT_Y, 240, WB_CONTENT_HEIGHT,
                         WB_COLOR_BG, 0);
    s_footer = make_box(s_screen, 0, WB_FOOTER_Y, 240, WB_FOOTER_HEIGHT,
                        WB_COLOR_DARK, 0);
    lv_screen_load(s_screen);
    return true;
}

void wb_ui_render(const wb_model_t *model, const wb_ui_runtime_t *runtime)
{
    if (model == NULL || runtime == NULL || s_screen == NULL) {
        return;
    }
    draw_header(model, runtime);
    lv_obj_clean(s_content);

    switch (model->screen) {
    case WB_SCREEN_PRIVACY_COVER:
        draw_privacy(model);
        break;
    case WB_SCREEN_INBOX:
        draw_inbox(model);
        break;
    case WB_SCREEN_TASKS:
        draw_tasks(model);
        break;
    case WB_SCREEN_OUTPUTS:
        draw_outputs(model);
        break;
    case WB_SCREEN_TASK_DETAIL:
        if (model->snapshot.task_count > 0U) {
            const wb_task_t *task = &model->snapshot.tasks[model->task_focus];
            draw_detail("任务详情", task->title, task_name(task->status), task->preview,
                        runtime->demo_mode
                            ? "确定：演示语音追问    长按：菜单"
                            : "云任务追问预留    长按：菜单");
        } else {
            draw_empty("任务详情", "任务已不在当前快照中");
            draw_footer("长按：菜单");
        }
        break;
    case WB_SCREEN_OUTPUT_DETAIL:
        if (model->snapshot.output_count > 0U) {
            const wb_output_t *output = &model->snapshot.outputs[model->output_focus];
            draw_detail("产出预览", output->title, output_name(output->kind),
                        output->preview, "长按：菜单");
        } else {
            draw_empty("产出预览", "产出已不在当前快照中");
            draw_footer("长按：菜单");
        }
        break;
    case WB_SCREEN_NAVIGATION:
        draw_navigation(model);
        break;
    case WB_SCREEN_VOICE_PREPARING:
        draw_centered_state("准备语音", "正在建立上传通道\n显示录音页后再开口",
                            WB_COLOR_WARNING, "长按：取消");
        break;
    case WB_SCREEN_RECORDING:
        draw_voice(model, runtime);
        break;
    case WB_SCREEN_TRANSCRIBING:
        draw_centered_state("正在转写", "语音只发送到你的网关\n不会直接执行任务",
                            WB_COLOR_WARNING, "长按：取消");
        break;
    case WB_SCREEN_REVIEW:
        draw_review(model);
        break;
    case WB_SCREEN_SUBMITTING:
        draw_centered_state("正在发送", "正在等待 WorkBuddy 收据",
                            WB_COLOR_ACCENT, "请稍候");
        break;
    case WB_SCREEN_RESULT: {
        char body[WB_ID_CAP + 32U];
        (void)snprintf(body, sizeof(body), "已送达\n收据 %s", model->receipt_id);
        draw_centered_state("发送成功", body, WB_COLOR_ACCENT,
                            "确定：返回");
        break;
    }
    case WB_SCREEN_ERROR: {
        char body[WB_TITLE_CAP + 40U];
        (void)snprintf(body, sizeof(body), "未确认送达\n%s", model->error_code);
        draw_centered_state("发送失败", body, WB_COLOR_DANGER,
                            model->retryable
                                ? "下：安全重试    确定：返回"
                                : "确定：返回");
        break;
    }
    case WB_SCREEN_STATUS:
        draw_status(model, runtime);
        break;
    default:
        draw_empty("WorkBuddy", runtime->notice != NULL ? runtime->notice : "状态更新中");
        draw_footer("长按：菜单");
        break;
    }

    if (runtime->notice != NULL && runtime->notice[0] != '\0') {
        lv_obj_t *toast = make_box(s_content, 12, 4, 216, 34,
                                   WB_COLOR_DARK, 10);
        lv_obj_t *label = make_label(toast, runtime->notice, 8, 7, 200,
                                     WB_COLOR_SURFACE,
                                     &lv_font_noto_sans_sc_14);
        set_one_line(label);
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_move_foreground(toast);
    }
}
