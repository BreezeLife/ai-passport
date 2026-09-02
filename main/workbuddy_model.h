#pragma once

#include "workbuddy_types.h"

typedef enum {
    WB_SCREEN_PRIVACY_COVER = 0,
    WB_SCREEN_INBOX,
    WB_SCREEN_TASKS,
    WB_SCREEN_OUTPUTS,
    WB_SCREEN_TASK_DETAIL,
    WB_SCREEN_OUTPUT_DETAIL,
    WB_SCREEN_NAVIGATION,
    WB_SCREEN_VOICE_PREPARING,
    WB_SCREEN_RECORDING,
    WB_SCREEN_TRANSCRIBING,
    WB_SCREEN_REVIEW,
    WB_SCREEN_SUBMITTING,
    WB_SCREEN_RESULT,
    WB_SCREEN_ERROR,
    WB_SCREEN_STATUS,
} wb_screen_t;

typedef enum {
    WB_MENU_INBOX = 0,
    WB_MENU_TASKS,
    WB_MENU_NEW_TASK,
    WB_MENU_OUTPUTS,
    WB_MENU_SYNC,
    WB_MENU_STATUS,
    WB_MENU_COUNT,
} wb_menu_item_t;

typedef enum {
    WB_VOICE_NONE = 0,
    WB_VOICE_REPLY,
    WB_VOICE_TASK_CREATE,
    WB_VOICE_TASK_FOLLOWUP,
} wb_voice_context_t;

typedef enum {
    WB_INPUT_UP_CLICK = 0,
    WB_INPUT_DOWN_CLICK,
    WB_INPUT_OK_CLICK,
    WB_INPUT_OK_LONG,
} wb_input_t;

typedef enum {
    WB_EFFECT_NONE = 0,
    WB_EFFECT_VOICE_STARTED,
    WB_EFFECT_VOICE_FINISHED,
    WB_EFFECT_VOICE_CANCELLED,
    WB_EFFECT_SUBMIT_REQUESTED,
    WB_EFFECT_SYNC_REQUESTED,
} wb_model_effect_t;

typedef struct {
    wb_snapshot_t snapshot;
    wb_screen_t screen;
    wb_screen_t return_screen;
    wb_screen_t navigation_return_screen;
    wb_menu_item_t menu_item;
    wb_voice_context_t voice_context;
    size_t inbox_focus;
    size_t task_focus;
    size_t output_focus;
    uint32_t recording_started_ms;
    uint32_t operation_nonce;
    uint32_t operation_sequence;
    char cursor[WB_ID_CAP];
    char operation_id[WB_ID_CAP];
    char target_id[WB_ID_CAP];
    char transcript[WB_TRANSCRIPT_CAP];
    char receipt_id[WB_ID_CAP];
    char error_code[WB_TITLE_CAP];
    bool cursor_seen;
    bool stale;
    bool submit_locked;
    bool retryable;
} wb_model_t;

void wb_model_init(wb_model_t *model, uint32_t operation_nonce);
bool wb_model_apply_snapshot(wb_model_t *model, const wb_snapshot_t *snapshot);
void wb_model_mark_stale(wb_model_t *model);
bool wb_model_begin_recording(wb_model_t *model,
                              const char *operation_id,
                              uint32_t now_ms);
wb_model_effect_t wb_model_handle_input(wb_model_t *model, wb_input_t input, uint32_t now_ms);
wb_model_effect_t wb_model_tick(wb_model_t *model, uint32_t now_ms);
bool wb_model_accept_transcript(wb_model_t *model, const char *transcript);
bool wb_model_pending_action(const wb_model_t *model, wb_action_t *action);
bool wb_model_complete_action(wb_model_t *model,
                              bool success,
                              const char *receipt_or_error,
                              bool retryable);
wb_task_status_t wb_task_status_normalize(const char *raw_status);
const char *wb_task_status_name(wb_task_status_t status);
