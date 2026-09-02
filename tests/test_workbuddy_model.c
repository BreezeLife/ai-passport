#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "workbuddy_model.h"

static unsigned s_tests_run;

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, #condition); \
        return 1; \
    } \
} while (0)

#define RUN_TEST(function) do { \
    int result = function(); \
    if (result != 0) { \
        return result; \
    } \
    ++s_tests_run; \
} while (0)

static wb_snapshot_t make_snapshot(const char *cursor)
{
    wb_snapshot_t snapshot = { 0 };

    snapshot.version = 1;
    snapshot.fresh = true;
    snapshot.assistant_available = true;
    snapshot.message_count = 3U;
    snapshot.task_count = 3U;
    snapshot.output_count = 3U;
    snprintf(snapshot.cursor, sizeof(snapshot.cursor), "%s", cursor);

    snprintf(snapshot.messages[0].id, sizeof(snapshot.messages[0].id), "message-1");
    snprintf(snapshot.messages[1].id, sizeof(snapshot.messages[1].id), "message-2");
    snprintf(snapshot.messages[2].id, sizeof(snapshot.messages[2].id), "message-3");
    snprintf(snapshot.tasks[0].id, sizeof(snapshot.tasks[0].id), "task-1");
    snprintf(snapshot.tasks[1].id, sizeof(snapshot.tasks[1].id), "task-2");
    snprintf(snapshot.tasks[2].id, sizeof(snapshot.tasks[2].id), "task-3");
    snprintf(snapshot.outputs[0].id, sizeof(snapshot.outputs[0].id), "output-1");
    snprintf(snapshot.outputs[1].id, sizeof(snapshot.outputs[1].id), "output-2");
    snprintf(snapshot.outputs[2].id, sizeof(snapshot.outputs[2].id), "output-3");
    return snapshot;
}

static int test_fixed_limits_and_initial_privacy_cover(void)
{
    wb_model_t model;

    CHECK(WB_MAX_ITEMS == 6U);
    CHECK(WB_ID_CAP == 65U);
    CHECK(WB_TITLE_CAP == 129U);
    CHECK(WB_PREVIEW_CAP == 385U);
    CHECK(WB_TRANSCRIPT_CAP == 513U);

    wb_model_init(&model, UINT32_C(0x1234));
    CHECK(model.screen == WB_SCREEN_PRIVACY_COVER);
    CHECK(model.stale);
    CHECK(model.voice_context == WB_VOICE_NONE);
    CHECK(!model.submit_locked);
    return 0;
}

static int test_privacy_cover_opens_inbox_without_exposing_content(void)
{
    wb_model_t model;
    wb_snapshot_t snapshot = make_snapshot("cursor-1");

    wb_model_init(&model, 1U);
    CHECK(wb_model_apply_snapshot(&model, &snapshot));
    CHECK(model.screen == WB_SCREEN_PRIVACY_COVER);
    CHECK(wb_model_handle_input(&model, WB_INPUT_OK_CLICK, 10U) == WB_EFFECT_NONE);
    CHECK(model.screen == WB_SCREEN_INBOX);
    return 0;
}

static int test_each_list_has_clamped_independent_focus(void)
{
    wb_model_t model;
    wb_snapshot_t snapshot = make_snapshot("cursor-1");

    wb_model_init(&model, 1U);
    (void)wb_model_apply_snapshot(&model, &snapshot);
    (void)wb_model_handle_input(&model, WB_INPUT_OK_CLICK, 0U);
    (void)wb_model_handle_input(&model, WB_INPUT_DOWN_CLICK, 0U);
    (void)wb_model_handle_input(&model, WB_INPUT_DOWN_CLICK, 0U);
    (void)wb_model_handle_input(&model, WB_INPUT_DOWN_CLICK, 0U);
    CHECK(model.inbox_focus == 2U);

    (void)wb_model_handle_input(&model, WB_INPUT_OK_LONG, 0U);
    CHECK(model.screen == WB_SCREEN_NAVIGATION);
    (void)wb_model_handle_input(&model, WB_INPUT_DOWN_CLICK, 0U);
    (void)wb_model_handle_input(&model, WB_INPUT_OK_CLICK, 0U);
    CHECK(model.screen == WB_SCREEN_TASKS);
    (void)wb_model_handle_input(&model, WB_INPUT_DOWN_CLICK, 0U);
    CHECK(model.task_focus == 1U);

    (void)wb_model_handle_input(&model, WB_INPUT_OK_LONG, 0U);
    (void)wb_model_handle_input(&model, WB_INPUT_DOWN_CLICK, 0U);
    (void)wb_model_handle_input(&model, WB_INPUT_DOWN_CLICK, 0U);
    (void)wb_model_handle_input(&model, WB_INPUT_OK_CLICK, 0U);
    CHECK(model.screen == WB_SCREEN_OUTPUTS);
    CHECK(model.output_focus == 0U);
    (void)wb_model_handle_input(&model, WB_INPUT_UP_CLICK, 0U);
    CHECK(model.output_focus == 0U);
    CHECK(model.inbox_focus == 2U);
    CHECK(model.task_focus == 1U);
    return 0;
}

static int test_empty_lists_do_not_underflow_focus(void)
{
    wb_model_t model;
    wb_snapshot_t snapshot = make_snapshot("cursor-1");

    snapshot.message_count = 0U;
    wb_model_init(&model, 1U);
    (void)wb_model_apply_snapshot(&model, &snapshot);
    (void)wb_model_handle_input(&model, WB_INPUT_OK_CLICK, 0U);
    (void)wb_model_handle_input(&model, WB_INPUT_UP_CLICK, 0U);
    (void)wb_model_handle_input(&model, WB_INPUT_DOWN_CLICK, 0U);
    CHECK(model.inbox_focus == 0U);
    CHECK(wb_model_handle_input(&model, WB_INPUT_OK_CLICK, 0U) == WB_EFFECT_NONE);
    CHECK(model.screen == WB_SCREEN_INBOX);
    return 0;
}

static int test_navigation_sheet_wraps_closes_and_requests_sync(void)
{
    wb_model_t model;
    wb_snapshot_t snapshot = make_snapshot("cursor-1");

    wb_model_init(&model, 1U);
    (void)wb_model_apply_snapshot(&model, &snapshot);
    (void)wb_model_handle_input(&model, WB_INPUT_OK_CLICK, 0U);
    (void)wb_model_handle_input(&model, WB_INPUT_OK_LONG, 0U);
    CHECK(model.menu_item == WB_MENU_INBOX);
    (void)wb_model_handle_input(&model, WB_INPUT_UP_CLICK, 0U);
    CHECK(model.menu_item == WB_MENU_STATUS);
    (void)wb_model_handle_input(&model, WB_INPUT_DOWN_CLICK, 0U);
    CHECK(model.menu_item == WB_MENU_INBOX);
    (void)wb_model_handle_input(&model, WB_INPUT_OK_LONG, 0U);
    CHECK(model.screen == WB_SCREEN_INBOX);

    (void)wb_model_handle_input(&model, WB_INPUT_OK_LONG, 0U);
    model.menu_item = WB_MENU_SYNC;
    CHECK(wb_model_handle_input(&model, WB_INPUT_OK_CLICK, 0U) == WB_EFFECT_SYNC_REQUESTED);
    CHECK(model.screen == WB_SCREEN_INBOX);
    return 0;
}

static int test_status_is_reachable_from_navigation(void)
{
    wb_model_t model;

    wb_model_init(&model, 1U);
    (void)wb_model_handle_input(&model, WB_INPUT_OK_CLICK, 0U);
    (void)wb_model_handle_input(&model, WB_INPUT_OK_LONG, 0U);
    model.menu_item = WB_MENU_STATUS;
    CHECK(wb_model_handle_input(&model, WB_INPUT_OK_CLICK, 0U) == WB_EFFECT_NONE);
    CHECK(model.screen == WB_SCREEN_STATUS);
    return 0;
}

static int test_cover_long_press_starts_contextual_new_task_voice(void)
{
    wb_model_t model;

    wb_model_init(&model, UINT32_C(0x10203040));
    CHECK(wb_model_handle_input(&model, WB_INPUT_OK_LONG, 55U) == WB_EFFECT_VOICE_STARTED);
    CHECK(model.screen == WB_SCREEN_RECORDING);
    CHECK(model.voice_context == WB_VOICE_TASK_CREATE);
    CHECK(model.return_screen == WB_SCREEN_PRIVACY_COVER);
    CHECK(model.target_id[0] == '\0');
    CHECK(model.operation_id[0] != '\0');
    return 0;
}

static int test_inbox_selection_starts_reply_for_focused_message(void)
{
    wb_model_t model;
    wb_snapshot_t snapshot = make_snapshot("cursor-1");

    wb_model_init(&model, 1U);
    (void)wb_model_apply_snapshot(&model, &snapshot);
    (void)wb_model_handle_input(&model, WB_INPUT_OK_CLICK, 0U);
    (void)wb_model_handle_input(&model, WB_INPUT_DOWN_CLICK, 0U);
    CHECK(wb_model_handle_input(&model, WB_INPUT_OK_CLICK, 100U) == WB_EFFECT_VOICE_STARTED);
    CHECK(model.voice_context == WB_VOICE_REPLY);
    CHECK(strcmp(model.target_id, "message-2") == 0);
    CHECK(model.return_screen == WB_SCREEN_INBOX);
    return 0;
}

static int test_task_detail_starts_followup_for_focused_task(void)
{
    wb_model_t model;
    wb_snapshot_t snapshot = make_snapshot("cursor-1");

    wb_model_init(&model, 1U);
    (void)wb_model_apply_snapshot(&model, &snapshot);
    (void)wb_model_handle_input(&model, WB_INPUT_OK_CLICK, 0U);
    (void)wb_model_handle_input(&model, WB_INPUT_OK_LONG, 0U);
    model.menu_item = WB_MENU_TASKS;
    (void)wb_model_handle_input(&model, WB_INPUT_OK_CLICK, 0U);
    (void)wb_model_handle_input(&model, WB_INPUT_DOWN_CLICK, 0U);
    CHECK(wb_model_handle_input(&model, WB_INPUT_OK_CLICK, 0U) == WB_EFFECT_NONE);
    CHECK(model.screen == WB_SCREEN_TASK_DETAIL);
    CHECK(wb_model_handle_input(&model, WB_INPUT_OK_CLICK, 200U) == WB_EFFECT_VOICE_STARTED);
    CHECK(model.voice_context == WB_VOICE_TASK_FOLLOWUP);
    CHECK(strcmp(model.target_id, "task-2") == 0);
    CHECK(model.return_screen == WB_SCREEN_TASK_DETAIL);
    return 0;
}

static int test_output_selection_opens_detail_without_voice(void)
{
    wb_model_t model;
    wb_snapshot_t snapshot = make_snapshot("cursor-1");

    wb_model_init(&model, 1U);
    (void)wb_model_apply_snapshot(&model, &snapshot);
    (void)wb_model_handle_input(&model, WB_INPUT_OK_CLICK, 0U);
    (void)wb_model_handle_input(&model, WB_INPUT_OK_LONG, 0U);
    model.menu_item = WB_MENU_OUTPUTS;
    (void)wb_model_handle_input(&model, WB_INPUT_OK_CLICK, 0U);
    (void)wb_model_handle_input(&model, WB_INPUT_DOWN_CLICK, 0U);
    CHECK(wb_model_handle_input(&model, WB_INPUT_OK_CLICK, 0U) == WB_EFFECT_NONE);
    CHECK(model.screen == WB_SCREEN_OUTPUT_DETAIL);
    CHECK(model.voice_context == WB_VOICE_NONE);
    return 0;
}

static int test_recording_stops_early_or_at_five_seconds_across_wrap(void)
{
    wb_model_t model;
    uint32_t start = UINT32_MAX - 1000U;

    wb_model_init(&model, 1U);
    (void)wb_model_handle_input(&model, WB_INPUT_OK_LONG, start);
    CHECK(wb_model_tick(&model, start + 4999U) == WB_EFFECT_NONE);
    CHECK(model.screen == WB_SCREEN_RECORDING);
    CHECK(wb_model_tick(&model, start + WB_RECORDING_LIMIT_MS) == WB_EFFECT_VOICE_FINISHED);
    CHECK(model.screen == WB_SCREEN_TRANSCRIBING);

    wb_model_init(&model, 1U);
    (void)wb_model_handle_input(&model, WB_INPUT_OK_LONG, 10U);
    CHECK(wb_model_handle_input(&model, WB_INPUT_OK_CLICK, 20U) == WB_EFFECT_VOICE_FINISHED);
    CHECK(model.screen == WB_SCREEN_TRANSCRIBING);
    return 0;
}

static int test_review_can_rerecord_with_new_operation_or_cancel(void)
{
    wb_model_t model;
    char first_operation[WB_ID_CAP];

    wb_model_init(&model, 7U);
    (void)wb_model_handle_input(&model, WB_INPUT_OK_LONG, 0U);
    (void)wb_model_handle_input(&model, WB_INPUT_OK_CLICK, 1U);
    CHECK(wb_model_accept_transcript(&model, "first draft"));
    CHECK(model.screen == WB_SCREEN_REVIEW);
    snprintf(first_operation, sizeof(first_operation), "%s", model.operation_id);
    CHECK(wb_model_handle_input(&model, WB_INPUT_DOWN_CLICK, 20U) == WB_EFFECT_VOICE_STARTED);
    CHECK(model.screen == WB_SCREEN_RECORDING);
    CHECK(model.transcript[0] == '\0');
    CHECK(strcmp(first_operation, model.operation_id) != 0);
    CHECK(wb_model_handle_input(&model, WB_INPUT_OK_LONG, 21U) == WB_EFFECT_VOICE_CANCELLED);
    CHECK(model.screen == WB_SCREEN_PRIVACY_COVER);

    (void)wb_model_handle_input(&model, WB_INPUT_OK_LONG, 30U);
    (void)wb_model_handle_input(&model, WB_INPUT_OK_CLICK, 31U);
    CHECK(wb_model_accept_transcript(&model, "cancel me"));
    CHECK(wb_model_handle_input(&model, WB_INPUT_OK_LONG, 32U) == WB_EFFECT_VOICE_CANCELLED);
    CHECK(model.screen == WB_SCREEN_PRIVACY_COVER);
    CHECK(model.transcript[0] == '\0');
    return 0;
}

static int test_review_submits_once_with_stable_action(void)
{
    wb_action_t action;
    wb_model_t model;
    wb_snapshot_t snapshot = make_snapshot("cursor-1");
    char operation_id[WB_ID_CAP];

    wb_model_init(&model, 9U);
    (void)wb_model_apply_snapshot(&model, &snapshot);
    (void)wb_model_handle_input(&model, WB_INPUT_OK_CLICK, 0U);
    (void)wb_model_handle_input(&model, WB_INPUT_OK_CLICK, 10U);
    (void)wb_model_handle_input(&model, WB_INPUT_OK_CLICK, 20U);
    CHECK(wb_model_accept_transcript(&model, "Ship the concise reply"));
    snprintf(operation_id, sizeof(operation_id), "%s", model.operation_id);

    CHECK(wb_model_handle_input(&model, WB_INPUT_OK_CLICK, 30U) == WB_EFFECT_SUBMIT_REQUESTED);
    CHECK(model.screen == WB_SCREEN_SUBMITTING);
    CHECK(model.submit_locked);
    CHECK(wb_model_pending_action(&model, &action));
    CHECK(action.type == WB_ACTION_REPLY);
    CHECK(strcmp(action.target_id, "message-1") == 0);
    CHECK(strcmp(action.text, "Ship the concise reply") == 0);
    CHECK(strcmp(action.operation_id, operation_id) == 0);
    CHECK(wb_model_handle_input(&model, WB_INPUT_OK_CLICK, 31U) == WB_EFFECT_NONE);
    CHECK(wb_model_pending_action(&model, &action));
    CHECK(strcmp(action.operation_id, operation_id) == 0);
    return 0;
}

static int test_cursor_deduplication_stale_and_focus_clamping(void)
{
    wb_model_t model;
    wb_snapshot_t first = make_snapshot("cursor-1");
    wb_snapshot_t second = make_snapshot("cursor-2");

    wb_model_init(&model, 1U);
    CHECK(wb_model_apply_snapshot(&model, &first));
    CHECK(!model.stale);
    CHECK(!wb_model_apply_snapshot(&model, &first));
    wb_model_mark_stale(&model);
    CHECK(model.stale);
    CHECK(!wb_model_apply_snapshot(&model, &first));
    CHECK(!model.stale);

    model.inbox_focus = 5U;
    second.message_count = WB_MAX_ITEMS + 4U;
    CHECK(wb_model_apply_snapshot(&model, &second));
    CHECK(model.snapshot.message_count == WB_MAX_ITEMS);
    CHECK(model.inbox_focus == WB_MAX_ITEMS - 1U);
    return 0;
}

static int test_workbuddy_statuses_are_normalized(void)
{
    CHECK(wb_task_status_normalize("CREATING") == WB_TASK_STATUS_QUEUED);
    CHECK(wb_task_status_normalize("idle") == WB_TASK_STATUS_QUEUED);
    CHECK(wb_task_status_normalize("planning") == WB_TASK_STATUS_RUNNING);
    CHECK(wb_task_status_normalize("working") == WB_TASK_STATUS_RUNNING);
    CHECK(wb_task_status_normalize("pending") == WB_TASK_STATUS_NEEDS_INPUT);
    CHECK(wb_task_status_normalize("completed") == WB_TASK_STATUS_COMPLETED);
    CHECK(wb_task_status_normalize("failed") == WB_TASK_STATUS_FAILED);
    CHECK(wb_task_status_normalize("archived") == WB_TASK_STATUS_UNKNOWN);
    CHECK(wb_task_status_normalize("deleted") == WB_TASK_STATUS_UNKNOWN);
    CHECK(wb_task_status_normalize("QUEUED") == WB_TASK_STATUS_QUEUED);
    CHECK(wb_task_status_normalize("RUNNING") == WB_TASK_STATUS_RUNNING);
    CHECK(wb_task_status_normalize("NEEDS_INPUT") == WB_TASK_STATUS_NEEDS_INPUT);
    CHECK(wb_task_status_normalize("CREATED") == WB_TASK_STATUS_UNKNOWN);
    CHECK(wb_task_status_normalize("mystery") == WB_TASK_STATUS_UNKNOWN);
    CHECK(strcmp(wb_task_status_name(WB_TASK_STATUS_NEEDS_INPUT), "NEEDS_INPUT") == 0);
    return 0;
}

static int test_invalid_utf8_transcripts_are_rejected(void)
{
    static const char invalid_lead[] = { (char)0x80, '\0' };
    static const char truncated[] = { (char)0xE2, (char)0x82, '\0' };
    static const char overlong[] = { (char)0xC0, (char)0xAF, '\0' };
    static const char surrogate[] = { (char)0xED, (char)0xA0, (char)0x80, '\0' };
    static const char too_high[] = {
        (char)0xF4, (char)0x90, (char)0x80, (char)0x80, '\0'
    };
    static const char maximum_valid[] = {
        (char)0xF4, (char)0x8F, (char)0xBF, (char)0xBF, '\0'
    };
    wb_model_t model;

    wb_model_init(&model, 1U);
    (void)wb_model_handle_input(&model, WB_INPUT_OK_LONG, 0U);
    (void)wb_model_handle_input(&model, WB_INPUT_OK_CLICK, 1U);
    CHECK(!wb_model_accept_transcript(&model, invalid_lead));
    CHECK(!wb_model_accept_transcript(&model, truncated));
    CHECK(!wb_model_accept_transcript(&model, overlong));
    CHECK(!wb_model_accept_transcript(&model, surrogate));
    CHECK(!wb_model_accept_transcript(&model, too_high));
    CHECK(model.screen == WB_SCREEN_TRANSCRIBING);
    CHECK(wb_model_accept_transcript(&model, maximum_valid));
    CHECK(model.screen == WB_SCREEN_REVIEW);
    return 0;
}

static int test_snapshot_fixed_arrays_are_sanitized_before_cursor_compare(void)
{
    wb_model_t model;
    wb_snapshot_t snapshot;

    memset(&snapshot, 0x7f, sizeof(snapshot));
    snapshot.version = 1U;
    snapshot.fresh = true;
    snapshot.assistant_available = true;
    snapshot.unread_count = UINT_MAX;
    snapshot.active_task_count = UINT_MAX;
    snapshot.message_count = SIZE_MAX;
    snapshot.task_count = SIZE_MAX;
    snapshot.output_count = SIZE_MAX;
    memset(snapshot.cursor, 'c', sizeof(snapshot.cursor));

    wb_model_init(&model, 1U);
    CHECK(wb_model_apply_snapshot(&model, &snapshot));
    CHECK(strlen(model.cursor) == WB_ID_MAX_BYTES);
    CHECK(model.snapshot.cursor[WB_ID_CAP - 1U] == '\0');
    CHECK(model.snapshot.messages[0].id[WB_ID_CAP - 1U] == '\0');
    CHECK(!wb_model_apply_snapshot(&model, &snapshot));
    return 0;
}

static void prepare_submitting_reply(wb_model_t *model, const wb_snapshot_t *snapshot)
{
    wb_model_init(model, 11U);
    (void)wb_model_apply_snapshot(model, snapshot);
    (void)wb_model_handle_input(model, WB_INPUT_OK_CLICK, 0U);
    (void)wb_model_handle_input(model, WB_INPUT_OK_CLICK, 1U);
    (void)wb_model_handle_input(model, WB_INPUT_OK_CLICK, 2U);
    (void)wb_model_accept_transcript(model, "confirmed text");
    (void)wb_model_handle_input(model, WB_INPUT_OK_CLICK, 3U);
}

static int test_success_result_is_bounded_and_returns_to_context(void)
{
    wb_model_t model;
    wb_snapshot_t snapshot = make_snapshot("cursor-1");
    char receipt[WB_ID_CAP + 20U];

    memset(receipt, 'r', sizeof(receipt) - 1U);
    receipt[sizeof(receipt) - 1U] = '\0';
    prepare_submitting_reply(&model, &snapshot);
    CHECK(wb_model_complete_action(&model, true, receipt, false));
    CHECK(model.screen == WB_SCREEN_RESULT);
    CHECK(!model.submit_locked);
    CHECK(strlen(model.receipt_id) == WB_ID_MAX_BYTES);
    CHECK(model.error_code[0] == '\0');
    CHECK(wb_model_handle_input(&model, WB_INPUT_OK_CLICK, 4U) == WB_EFFECT_NONE);
    CHECK(model.screen == WB_SCREEN_INBOX);
    CHECK(model.voice_context == WB_VOICE_NONE);
    return 0;
}

static int test_failure_requires_explicit_retry_with_same_operation_id(void)
{
    wb_model_t model;
    wb_snapshot_t snapshot = make_snapshot("cursor-1");
    char error_code[WB_TITLE_CAP + 20U];
    char operation_id[WB_ID_CAP];

    memset(error_code, 'e', sizeof(error_code) - 1U);
    error_code[sizeof(error_code) - 1U] = '\0';
    prepare_submitting_reply(&model, &snapshot);
    snprintf(operation_id, sizeof(operation_id), "%s", model.operation_id);
    CHECK(wb_model_complete_action(&model, false, error_code, true));
    CHECK(model.screen == WB_SCREEN_ERROR);
    CHECK(model.submit_locked);
    CHECK(model.retryable);
    CHECK(strlen(model.error_code) == WB_TITLE_MAX_BYTES);
    CHECK(model.receipt_id[0] == '\0');

    CHECK(wb_model_handle_input(&model, WB_INPUT_OK_CLICK, 4U) == WB_EFFECT_NONE);
    CHECK(model.screen == WB_SCREEN_INBOX);

    prepare_submitting_reply(&model, &snapshot);
    snprintf(operation_id, sizeof(operation_id), "%s", model.operation_id);
    CHECK(wb_model_complete_action(&model, false, "TIMEOUT", true));
    CHECK(wb_model_handle_input(&model, WB_INPUT_DOWN_CLICK, 5U) ==
          WB_EFFECT_SUBMIT_REQUESTED);
    CHECK(model.screen == WB_SCREEN_SUBMITTING);
    CHECK(strcmp(model.operation_id, operation_id) == 0);
    return 0;
}

static int test_nonretryable_failure_and_long_press_only_return(void)
{
    wb_model_t model;
    wb_snapshot_t snapshot = make_snapshot("cursor-1");

    prepare_submitting_reply(&model, &snapshot);
    CHECK(wb_model_complete_action(&model, false, "AUTH", false));
    CHECK(!model.retryable);
    CHECK(wb_model_handle_input(&model, WB_INPUT_DOWN_CLICK, 4U) == WB_EFFECT_NONE);
    CHECK(model.screen == WB_SCREEN_ERROR);
    CHECK(wb_model_handle_input(&model, WB_INPUT_OK_LONG, 5U) == WB_EFFECT_NONE);
    CHECK(model.screen == WB_SCREEN_INBOX);
    return 0;
}

static int test_detail_long_press_navigation_returns_to_same_detail(void)
{
    wb_model_t model;
    wb_snapshot_t snapshot = make_snapshot("cursor-1");

    wb_model_init(&model, 1U);
    (void)wb_model_apply_snapshot(&model, &snapshot);
    (void)wb_model_handle_input(&model, WB_INPUT_OK_CLICK, 0U);
    (void)wb_model_handle_input(&model, WB_INPUT_OK_LONG, 0U);
    model.menu_item = WB_MENU_TASKS;
    (void)wb_model_handle_input(&model, WB_INPUT_OK_CLICK, 0U);
    (void)wb_model_handle_input(&model, WB_INPUT_OK_CLICK, 0U);
    CHECK(model.screen == WB_SCREEN_TASK_DETAIL);
    (void)wb_model_handle_input(&model, WB_INPUT_OK_LONG, 0U);
    CHECK(model.screen == WB_SCREEN_NAVIGATION);
    CHECK(model.menu_item == WB_MENU_TASKS);
    (void)wb_model_handle_input(&model, WB_INPUT_OK_LONG, 0U);
    CHECK(model.screen == WB_SCREEN_TASK_DETAIL);

    (void)wb_model_handle_input(&model, WB_INPUT_OK_LONG, 0U);
    model.menu_item = WB_MENU_OUTPUTS;
    (void)wb_model_handle_input(&model, WB_INPUT_OK_CLICK, 0U);
    (void)wb_model_handle_input(&model, WB_INPUT_OK_CLICK, 0U);
    CHECK(model.screen == WB_SCREEN_OUTPUT_DETAIL);
    (void)wb_model_handle_input(&model, WB_INPUT_OK_LONG, 0U);
    CHECK(model.screen == WB_SCREEN_NAVIGATION);
    CHECK(model.menu_item == WB_MENU_OUTPUTS);
    (void)wb_model_handle_input(&model, WB_INPUT_OK_LONG, 0U);
    CHECK(model.screen == WB_SCREEN_OUTPUT_DETAIL);
    return 0;
}

int main(void)
{
    RUN_TEST(test_fixed_limits_and_initial_privacy_cover);
    RUN_TEST(test_privacy_cover_opens_inbox_without_exposing_content);
    RUN_TEST(test_each_list_has_clamped_independent_focus);
    RUN_TEST(test_empty_lists_do_not_underflow_focus);
    RUN_TEST(test_navigation_sheet_wraps_closes_and_requests_sync);
    RUN_TEST(test_status_is_reachable_from_navigation);
    RUN_TEST(test_cover_long_press_starts_contextual_new_task_voice);
    RUN_TEST(test_inbox_selection_starts_reply_for_focused_message);
    RUN_TEST(test_task_detail_starts_followup_for_focused_task);
    RUN_TEST(test_output_selection_opens_detail_without_voice);
    RUN_TEST(test_recording_stops_early_or_at_five_seconds_across_wrap);
    RUN_TEST(test_review_can_rerecord_with_new_operation_or_cancel);
    RUN_TEST(test_review_submits_once_with_stable_action);
    RUN_TEST(test_cursor_deduplication_stale_and_focus_clamping);
    RUN_TEST(test_workbuddy_statuses_are_normalized);
    RUN_TEST(test_invalid_utf8_transcripts_are_rejected);
    RUN_TEST(test_snapshot_fixed_arrays_are_sanitized_before_cursor_compare);
    RUN_TEST(test_success_result_is_bounded_and_returns_to_context);
    RUN_TEST(test_failure_requires_explicit_retry_with_same_operation_id);
    RUN_TEST(test_nonretryable_failure_and_long_press_only_return);
    RUN_TEST(test_detail_long_press_navigation_returns_to_same_detail);
    printf("workbuddy model: %u tests passed\n", s_tests_run);
    return 0;
}
