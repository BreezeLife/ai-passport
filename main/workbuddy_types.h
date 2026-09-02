#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define WB_MAX_ITEMS 6U

/* Capacities include the trailing NUL byte. */
#define WB_ID_CAP 65U
#define WB_TITLE_CAP 129U
#define WB_PREVIEW_CAP 385U
#define WB_TRANSCRIPT_CAP 513U

#define WB_ID_CAPACITY WB_ID_CAP
#define WB_TITLE_CAPACITY WB_TITLE_CAP
#define WB_PREVIEW_CAPACITY WB_PREVIEW_CAP
#define WB_TRANSCRIPT_CAPACITY WB_TRANSCRIPT_CAP

#define WB_ID_MAX_BYTES (WB_ID_CAP - 1U)
#define WB_TITLE_MAX_BYTES (WB_TITLE_CAP - 1U)
#define WB_PREVIEW_MAX_BYTES (WB_PREVIEW_CAP - 1U)
#define WB_TRANSCRIPT_MAX_BYTES (WB_TRANSCRIPT_CAP - 1U)
#define WB_SNAPSHOT_MAX_BYTES (12U * 1024U)
#define WB_RECORDING_LIMIT_MS 5000U

typedef enum {
    WB_MESSAGE_ROLE_UNKNOWN = 0,
    WB_MESSAGE_ROLE_ASSISTANT,
    WB_MESSAGE_ROLE_USER,
    WB_MESSAGE_ROLE_SYSTEM,
} wb_message_role_t;

typedef enum {
    WB_TASK_STATUS_UNKNOWN = 0,
    WB_TASK_STATUS_QUEUED,
    WB_TASK_STATUS_RUNNING,
    WB_TASK_STATUS_NEEDS_INPUT,
    WB_TASK_STATUS_COMPLETED,
    WB_TASK_STATUS_FAILED,
} wb_task_status_t;

typedef enum {
    WB_OUTPUT_KIND_UNKNOWN = 0,
    WB_OUTPUT_KIND_PLAN,
    WB_OUTPUT_KIND_CHECKLIST,
    WB_OUTPUT_KIND_OVERVIEW,
    WB_OUTPUT_KIND_IMAGE,
    WB_OUTPUT_KIND_DOCUMENT,
} wb_output_kind_t;

typedef struct {
    char id[WB_ID_CAP];
    char title[WB_TITLE_CAP];
    char preview[WB_PREVIEW_CAP];
    wb_message_role_t role;
    bool unread;
} wb_message_t;

typedef struct {
    char id[WB_ID_CAP];
    char title[WB_TITLE_CAP];
    char preview[WB_PREVIEW_CAP];
    wb_task_status_t status;
} wb_task_t;

typedef struct {
    char id[WB_ID_CAP];
    char task_id[WB_ID_CAP];
    char title[WB_TITLE_CAP];
    char preview[WB_PREVIEW_CAP];
    wb_output_kind_t kind;
} wb_output_t;

typedef struct {
    unsigned version;
    char request_id[WB_ID_CAP];
    char cursor[WB_ID_CAP];
    bool fresh;
    bool assistant_available;
    unsigned unread_count;
    unsigned active_task_count;
    size_t message_count;
    size_t task_count;
    size_t output_count;
    wb_message_t messages[WB_MAX_ITEMS];
    wb_task_t tasks[WB_MAX_ITEMS];
    wb_output_t outputs[WB_MAX_ITEMS];
} wb_snapshot_t;

typedef enum {
    WB_ACTION_REPLY = 0,
    WB_ACTION_TASK_CREATE,
    WB_ACTION_TASK_FOLLOWUP,
} wb_action_type_t;

typedef struct {
    wb_action_type_t type;
    char operation_id[WB_ID_CAP];
    char target_id[WB_ID_CAP];
    char text[WB_TRANSCRIPT_CAP];
} wb_action_t;
