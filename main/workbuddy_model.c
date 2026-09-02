#include "workbuddy_model.h"

#include <ctype.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

static size_t utf8_codepoint_bytes(const unsigned char *text, size_t remaining)
{
    if (text[0] < 0x80U) {
        return 1U;
    }
    if (remaining >= 2U && text[0] >= 0xC2U && text[0] <= 0xDFU &&
        text[1] >= 0x80U && text[1] <= 0xBFU) {
        return 2U;
    }
    if (remaining >= 3U && text[0] == 0xE0U &&
        text[1] >= 0xA0U && text[1] <= 0xBFU &&
        text[2] >= 0x80U && text[2] <= 0xBFU) {
        return 3U;
    }
    if (remaining >= 3U &&
        ((text[0] >= 0xE1U && text[0] <= 0xECU) ||
         (text[0] >= 0xEEU && text[0] <= 0xEFU)) &&
        text[1] >= 0x80U && text[1] <= 0xBFU &&
        text[2] >= 0x80U && text[2] <= 0xBFU) {
        return 3U;
    }
    if (remaining >= 3U && text[0] == 0xEDU &&
        text[1] >= 0x80U && text[1] <= 0x9FU &&
        text[2] >= 0x80U && text[2] <= 0xBFU) {
        return 3U;
    }
    if (remaining >= 4U && text[0] == 0xF0U &&
        text[1] >= 0x90U && text[1] <= 0xBFU &&
        text[2] >= 0x80U && text[2] <= 0xBFU &&
        text[3] >= 0x80U && text[3] <= 0xBFU) {
        return 4U;
    }
    if (remaining >= 4U && text[0] >= 0xF1U && text[0] <= 0xF3U &&
        text[1] >= 0x80U && text[1] <= 0xBFU &&
        text[2] >= 0x80U && text[2] <= 0xBFU &&
        text[3] >= 0x80U && text[3] <= 0xBFU) {
        return 4U;
    }
    if (remaining >= 4U && text[0] == 0xF4U &&
        text[1] >= 0x80U && text[1] <= 0x8FU &&
        text[2] >= 0x80U && text[2] <= 0xBFU &&
        text[3] >= 0x80U && text[3] <= 0xBFU) {
        return 4U;
    }
    return 0U;
}

static bool utf8_is_valid(const char *text, size_t length)
{
    size_t offset = 0U;

    while (offset < length) {
        size_t codepoint_bytes = utf8_codepoint_bytes(
            (const unsigned char *)&text[offset], length - offset);

        if (codepoint_bytes == 0U) {
            return false;
        }
        offset += codepoint_bytes;
    }
    return true;
}

static void copy_utf8(char *destination, size_t capacity, const char *source)
{
    size_t read_index = 0U;
    size_t write_index = 0U;
    size_t source_length;

    if (capacity == 0U) {
        return;
    }
    if (source == NULL) {
        destination[0] = '\0';
        return;
    }
    source_length = strlen(source);

    while (read_index < source_length) {
        size_t codepoint_bytes = utf8_codepoint_bytes(
            (const unsigned char *)&source[read_index], source_length - read_index);

        if (codepoint_bytes == 0U) {
            break;
        }
        if (write_index + codepoint_bytes >= capacity) {
            break;
        }
        memmove(&destination[write_index], &source[read_index], codepoint_bytes);
        read_index += codepoint_bytes;
        write_index += codepoint_bytes;
    }
    destination[write_index] = '\0';
}

static void terminate_snapshot_strings(wb_snapshot_t *snapshot)
{
    size_t index;

    snapshot->request_id[WB_ID_CAP - 1U] = '\0';
    snapshot->cursor[WB_ID_CAP - 1U] = '\0';
    for (index = 0U; index < WB_MAX_ITEMS; ++index) {
        snapshot->messages[index].id[WB_ID_CAP - 1U] = '\0';
        snapshot->messages[index].title[WB_TITLE_CAP - 1U] = '\0';
        snapshot->messages[index].preview[WB_PREVIEW_CAP - 1U] = '\0';
    }
    for (index = 0U; index < WB_MAX_ITEMS; ++index) {
        snapshot->tasks[index].id[WB_ID_CAP - 1U] = '\0';
        snapshot->tasks[index].title[WB_TITLE_CAP - 1U] = '\0';
        snapshot->tasks[index].preview[WB_PREVIEW_CAP - 1U] = '\0';
    }
    for (index = 0U; index < WB_MAX_ITEMS; ++index) {
        snapshot->outputs[index].id[WB_ID_CAP - 1U] = '\0';
        snapshot->outputs[index].task_id[WB_ID_CAP - 1U] = '\0';
        snapshot->outputs[index].title[WB_TITLE_CAP - 1U] = '\0';
        snapshot->outputs[index].preview[WB_PREVIEW_CAP - 1U] = '\0';
    }
}

static size_t clamp_focus(size_t focus, size_t count)
{
    if (count == 0U) {
        return 0U;
    }
    return focus < count ? focus : count - 1U;
}

static void next_operation_id(wb_model_t *model)
{
    ++model->operation_sequence;
    if (model->operation_sequence == 0U) {
        ++model->operation_sequence;
    }
    (void)snprintf(model->operation_id, sizeof(model->operation_id),
                   "wb-%08" PRIx32 "-%08" PRIx32,
                   model->operation_nonce, model->operation_sequence);
}

static wb_model_effect_t start_voice(wb_model_t *model,
                                     wb_voice_context_t context,
                                     const char *target_id,
                                     wb_screen_t return_screen,
                                     uint32_t now_ms)
{
    model->voice_context = context;
    model->return_screen = return_screen;
    model->recording_started_ms = now_ms;
    model->transcript[0] = '\0';
    model->receipt_id[0] = '\0';
    model->error_code[0] = '\0';
    model->submit_locked = false;
    model->retryable = false;
    copy_utf8(model->target_id, sizeof(model->target_id), target_id);
    next_operation_id(model);
    model->screen = WB_SCREEN_RECORDING;
    return WB_EFFECT_VOICE_STARTED;
}

static void clear_voice(wb_model_t *model)
{
    model->voice_context = WB_VOICE_NONE;
    model->operation_id[0] = '\0';
    model->target_id[0] = '\0';
    model->transcript[0] = '\0';
    model->receipt_id[0] = '\0';
    model->error_code[0] = '\0';
    model->submit_locked = false;
    model->retryable = false;
}

static wb_menu_item_t menu_for_screen(wb_screen_t screen)
{
    switch (screen) {
    case WB_SCREEN_TASKS:
    case WB_SCREEN_TASK_DETAIL:
        return WB_MENU_TASKS;
    case WB_SCREEN_OUTPUTS:
    case WB_SCREEN_OUTPUT_DETAIL:
        return WB_MENU_OUTPUTS;
    case WB_SCREEN_STATUS:
        return WB_MENU_STATUS;
    default:
        return WB_MENU_INBOX;
    }
}

static void open_navigation(wb_model_t *model)
{
    model->navigation_return_screen = model->screen;
    model->menu_item = menu_for_screen(model->screen);
    model->screen = WB_SCREEN_NAVIGATION;
}

static void move_focus(size_t *focus, size_t count, int direction)
{
    if (direction < 0) {
        if (*focus > 0U) {
            --*focus;
        }
    } else if (*focus + 1U < count) {
        ++*focus;
    }
}

static wb_model_effect_t handle_navigation(wb_model_t *model,
                                           wb_input_t input,
                                           uint32_t now_ms)
{
    if (input == WB_INPUT_UP_CLICK) {
        model->menu_item = model->menu_item == WB_MENU_INBOX
            ? WB_MENU_STATUS
            : (wb_menu_item_t)(model->menu_item - 1);
        return WB_EFFECT_NONE;
    }
    if (input == WB_INPUT_DOWN_CLICK) {
        model->menu_item = model->menu_item == WB_MENU_STATUS
            ? WB_MENU_INBOX
            : (wb_menu_item_t)(model->menu_item + 1);
        return WB_EFFECT_NONE;
    }
    if (input == WB_INPUT_OK_LONG) {
        model->screen = model->navigation_return_screen;
        return WB_EFFECT_NONE;
    }
    if (input != WB_INPUT_OK_CLICK) {
        return WB_EFFECT_NONE;
    }

    switch (model->menu_item) {
    case WB_MENU_INBOX:
        model->screen = WB_SCREEN_INBOX;
        return WB_EFFECT_NONE;
    case WB_MENU_TASKS:
        model->screen = WB_SCREEN_TASKS;
        return WB_EFFECT_NONE;
    case WB_MENU_NEW_TASK:
        return start_voice(model, WB_VOICE_TASK_CREATE, "",
                           model->navigation_return_screen, now_ms);
    case WB_MENU_OUTPUTS:
        model->screen = WB_SCREEN_OUTPUTS;
        return WB_EFFECT_NONE;
    case WB_MENU_SYNC:
        model->screen = model->navigation_return_screen;
        return WB_EFFECT_SYNC_REQUESTED;
    case WB_MENU_STATUS:
        model->screen = WB_SCREEN_STATUS;
        return WB_EFFECT_NONE;
    default:
        return WB_EFFECT_NONE;
    }
}

void wb_model_init(wb_model_t *model, uint32_t operation_nonce)
{
    if (model == NULL) {
        return;
    }
    memset(model, 0, sizeof(*model));
    model->screen = WB_SCREEN_PRIVACY_COVER;
    model->return_screen = WB_SCREEN_PRIVACY_COVER;
    model->navigation_return_screen = WB_SCREEN_PRIVACY_COVER;
    model->operation_nonce = operation_nonce;
    model->stale = true;
}

bool wb_model_apply_snapshot(wb_model_t *model, const wb_snapshot_t *snapshot)
{
    bool cursor_advanced;
    wb_snapshot_t sanitized;

    if (model == NULL || snapshot == NULL) {
        return false;
    }

    sanitized = *snapshot;
    if (sanitized.message_count > WB_MAX_ITEMS) {
        sanitized.message_count = WB_MAX_ITEMS;
    }
    if (sanitized.task_count > WB_MAX_ITEMS) {
        sanitized.task_count = WB_MAX_ITEMS;
    }
    if (sanitized.output_count > WB_MAX_ITEMS) {
        sanitized.output_count = WB_MAX_ITEMS;
    }
    terminate_snapshot_strings(&sanitized);

    cursor_advanced = sanitized.cursor[0] != '\0' &&
        (!model->cursor_seen || strcmp(model->cursor, sanitized.cursor) != 0);
    model->snapshot = sanitized;

    if (sanitized.cursor[0] != '\0') {
        memcpy(model->cursor, sanitized.cursor, sizeof(model->cursor));
        model->cursor_seen = true;
    }
    model->inbox_focus = clamp_focus(model->inbox_focus, model->snapshot.message_count);
    model->task_focus = clamp_focus(model->task_focus, model->snapshot.task_count);
    model->output_focus = clamp_focus(model->output_focus, model->snapshot.output_count);
    model->stale = !sanitized.fresh;
    return cursor_advanced;
}

void wb_model_mark_stale(wb_model_t *model)
{
    if (model != NULL) {
        model->stale = true;
    }
}

wb_model_effect_t wb_model_handle_input(wb_model_t *model,
                                        wb_input_t input,
                                        uint32_t now_ms)
{
    if (model == NULL) {
        return WB_EFFECT_NONE;
    }

    if (model->screen == WB_SCREEN_NAVIGATION) {
        return handle_navigation(model, input, now_ms);
    }

    if (model->screen == WB_SCREEN_RECORDING) {
        if (input == WB_INPUT_OK_CLICK) {
            model->screen = WB_SCREEN_TRANSCRIBING;
            return WB_EFFECT_VOICE_FINISHED;
        }
        if (input == WB_INPUT_OK_LONG) {
            model->screen = model->return_screen;
            clear_voice(model);
            return WB_EFFECT_VOICE_CANCELLED;
        }
        return WB_EFFECT_NONE;
    }

    if (model->screen == WB_SCREEN_TRANSCRIBING) {
        if (input == WB_INPUT_OK_LONG) {
            model->screen = model->return_screen;
            clear_voice(model);
            return WB_EFFECT_VOICE_CANCELLED;
        }
        return WB_EFFECT_NONE;
    }

    if (model->screen == WB_SCREEN_REVIEW) {
        if (input == WB_INPUT_DOWN_CLICK) {
            return start_voice(model, model->voice_context, model->target_id,
                               model->return_screen, now_ms);
        }
        if (input == WB_INPUT_OK_LONG) {
            model->screen = model->return_screen;
            clear_voice(model);
            return WB_EFFECT_VOICE_CANCELLED;
        }
        if (input == WB_INPUT_OK_CLICK && !model->submit_locked) {
            model->submit_locked = true;
            model->screen = WB_SCREEN_SUBMITTING;
            return WB_EFFECT_SUBMIT_REQUESTED;
        }
        return WB_EFFECT_NONE;
    }

    if (model->screen == WB_SCREEN_SUBMITTING) {
        return WB_EFFECT_NONE;
    }

    if (model->screen == WB_SCREEN_RESULT) {
        if (input == WB_INPUT_OK_CLICK || input == WB_INPUT_OK_LONG) {
            model->screen = model->return_screen;
            clear_voice(model);
        }
        return WB_EFFECT_NONE;
    }

    if (model->screen == WB_SCREEN_ERROR) {
        if (input == WB_INPUT_DOWN_CLICK && model->retryable) {
            model->screen = WB_SCREEN_SUBMITTING;
            return WB_EFFECT_SUBMIT_REQUESTED;
        }
        if (input == WB_INPUT_OK_CLICK || input == WB_INPUT_OK_LONG) {
            model->screen = model->return_screen;
            clear_voice(model);
        }
        return WB_EFFECT_NONE;
    }

    if (model->screen == WB_SCREEN_PRIVACY_COVER) {
        if (input == WB_INPUT_OK_CLICK) {
            model->screen = WB_SCREEN_INBOX;
        } else if (input == WB_INPUT_OK_LONG) {
            return start_voice(model, WB_VOICE_TASK_CREATE, "",
                               WB_SCREEN_PRIVACY_COVER, now_ms);
        }
        return WB_EFFECT_NONE;
    }

    if (input == WB_INPUT_OK_LONG) {
        open_navigation(model);
        return WB_EFFECT_NONE;
    }

    if (model->screen == WB_SCREEN_INBOX) {
        if (input == WB_INPUT_UP_CLICK) {
            move_focus(&model->inbox_focus, model->snapshot.message_count, -1);
        } else if (input == WB_INPUT_DOWN_CLICK) {
            move_focus(&model->inbox_focus, model->snapshot.message_count, 1);
        } else if (input == WB_INPUT_OK_CLICK && model->snapshot.message_count > 0U) {
            return start_voice(model, WB_VOICE_REPLY,
                               model->snapshot.messages[model->inbox_focus].id,
                               WB_SCREEN_INBOX, now_ms);
        }
    } else if (model->screen == WB_SCREEN_TASKS) {
        if (input == WB_INPUT_UP_CLICK) {
            move_focus(&model->task_focus, model->snapshot.task_count, -1);
        } else if (input == WB_INPUT_DOWN_CLICK) {
            move_focus(&model->task_focus, model->snapshot.task_count, 1);
        } else if (input == WB_INPUT_OK_CLICK && model->snapshot.task_count > 0U) {
            model->screen = WB_SCREEN_TASK_DETAIL;
        }
    } else if (model->screen == WB_SCREEN_TASK_DETAIL && input == WB_INPUT_OK_CLICK &&
               model->snapshot.task_count > 0U) {
        return start_voice(model, WB_VOICE_TASK_FOLLOWUP,
                           model->snapshot.tasks[model->task_focus].id,
                           WB_SCREEN_TASK_DETAIL, now_ms);
    } else if (model->screen == WB_SCREEN_OUTPUTS) {
        if (input == WB_INPUT_UP_CLICK) {
            move_focus(&model->output_focus, model->snapshot.output_count, -1);
        } else if (input == WB_INPUT_DOWN_CLICK) {
            move_focus(&model->output_focus, model->snapshot.output_count, 1);
        } else if (input == WB_INPUT_OK_CLICK && model->snapshot.output_count > 0U) {
            model->screen = WB_SCREEN_OUTPUT_DETAIL;
        }
    }
    return WB_EFFECT_NONE;
}

wb_model_effect_t wb_model_tick(wb_model_t *model, uint32_t now_ms)
{
    if (model == NULL || model->screen != WB_SCREEN_RECORDING) {
        return WB_EFFECT_NONE;
    }
    if ((uint32_t)(now_ms - model->recording_started_ms) < WB_RECORDING_LIMIT_MS) {
        return WB_EFFECT_NONE;
    }
    model->screen = WB_SCREEN_TRANSCRIBING;
    return WB_EFFECT_VOICE_FINISHED;
}

bool wb_model_accept_transcript(wb_model_t *model, const char *transcript)
{
    size_t transcript_length;

    if (model == NULL || transcript == NULL || transcript[0] == '\0' ||
        model->screen != WB_SCREEN_TRANSCRIBING) {
        return false;
    }
    transcript_length = strlen(transcript);
    if (!utf8_is_valid(transcript, transcript_length)) {
        return false;
    }
    copy_utf8(model->transcript, sizeof(model->transcript), transcript);
    if (model->transcript[0] == '\0') {
        return false;
    }
    model->screen = WB_SCREEN_REVIEW;
    return true;
}

bool wb_model_pending_action(const wb_model_t *model, wb_action_t *action)
{
    if (model == NULL || action == NULL || !model->submit_locked ||
        model->screen != WB_SCREEN_SUBMITTING) {
        return false;
    }

    memset(action, 0, sizeof(*action));
    switch (model->voice_context) {
    case WB_VOICE_REPLY:
        action->type = WB_ACTION_REPLY;
        break;
    case WB_VOICE_TASK_CREATE:
        action->type = WB_ACTION_TASK_CREATE;
        break;
    case WB_VOICE_TASK_FOLLOWUP:
        action->type = WB_ACTION_TASK_FOLLOWUP;
        break;
    default:
        return false;
    }
    copy_utf8(action->operation_id, sizeof(action->operation_id), model->operation_id);
    copy_utf8(action->target_id, sizeof(action->target_id), model->target_id);
    copy_utf8(action->text, sizeof(action->text), model->transcript);
    return true;
}

bool wb_model_complete_action(wb_model_t *model,
                              bool success,
                              const char *receipt_or_error,
                              bool retryable)
{
    if (model == NULL || receipt_or_error == NULL || receipt_or_error[0] == '\0' ||
        model->screen != WB_SCREEN_SUBMITTING || !model->submit_locked) {
        return false;
    }

    if (success) {
        copy_utf8(model->receipt_id, sizeof(model->receipt_id), receipt_or_error);
        model->error_code[0] = '\0';
        model->retryable = false;
        model->submit_locked = false;
        model->screen = WB_SCREEN_RESULT;
    } else {
        model->receipt_id[0] = '\0';
        copy_utf8(model->error_code, sizeof(model->error_code), receipt_or_error);
        model->retryable = retryable;
        model->screen = WB_SCREEN_ERROR;
    }
    return true;
}

static void canonicalize_status(char *destination, size_t capacity, const char *source)
{
    size_t write_index = 0U;

    if (source == NULL || capacity == 0U) {
        if (capacity > 0U) {
            destination[0] = '\0';
        }
        return;
    }
    while (*source != '\0' && write_index + 1U < capacity) {
        unsigned char byte = (unsigned char)*source++;

        destination[write_index++] = (char)toupper(byte);
    }
    destination[write_index] = '\0';
}

static bool status_is(const char *status, const char *candidate)
{
    return strcmp(status, candidate) == 0;
}

wb_task_status_t wb_task_status_normalize(const char *raw_status)
{
    char status[32];

    canonicalize_status(status, sizeof(status), raw_status);
    if (status_is(status, "QUEUED") || status_is(status, "CREATING") ||
        status_is(status, "IDLE")) {
        return WB_TASK_STATUS_QUEUED;
    }
    if (status_is(status, "RUNNING") || status_is(status, "PLANNING") ||
        status_is(status, "WORKING")) {
        return WB_TASK_STATUS_RUNNING;
    }
    if (status_is(status, "NEEDS_INPUT") || status_is(status, "PENDING")) {
        return WB_TASK_STATUS_NEEDS_INPUT;
    }
    if (status_is(status, "COMPLETED")) {
        return WB_TASK_STATUS_COMPLETED;
    }
    if (status_is(status, "FAILED")) {
        return WB_TASK_STATUS_FAILED;
    }
    return WB_TASK_STATUS_UNKNOWN;
}

const char *wb_task_status_name(wb_task_status_t status)
{
    switch (status) {
    case WB_TASK_STATUS_QUEUED:
        return "QUEUED";
    case WB_TASK_STATUS_RUNNING:
        return "RUNNING";
    case WB_TASK_STATUS_NEEDS_INPUT:
        return "NEEDS_INPUT";
    case WB_TASK_STATUS_COMPLETED:
        return "COMPLETED";
    case WB_TASK_STATUS_FAILED:
        return "FAILED";
    default:
        return "UNKNOWN";
    }
}
