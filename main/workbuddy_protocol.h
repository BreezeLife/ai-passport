#pragma once

#include <stddef.h>

#include "workbuddy_types.h"

typedef enum {
    WB_PROTOCOL_OK = 0,
    WB_PROTOCOL_ERR_ARGUMENT,
    WB_PROTOCOL_ERR_INVALID_JSON,
    WB_PROTOCOL_ERR_UNSUPPORTED_VERSION,
    WB_PROTOCOL_ERR_MISSING_FIELD,
    WB_PROTOCOL_ERR_TYPE,
    WB_PROTOCOL_ERR_LIMIT,
    WB_PROTOCOL_ERR_BUFFER_TOO_SMALL,
} wb_protocol_result_t;

wb_protocol_result_t wb_protocol_parse_snapshot(const char *json,
                                                size_t json_length,
                                                wb_snapshot_t *snapshot);

wb_protocol_result_t wb_protocol_serialize_action(const wb_action_t *action,
                                                  char *output,
                                                  size_t output_capacity,
                                                  size_t *output_length);
wb_protocol_result_t wb_protocol_serialize_reply(const char *operation_id,
                                                 const char *message_id,
                                                 const char *text,
                                                 char *output,
                                                 size_t output_capacity,
                                                 size_t *output_length);
wb_protocol_result_t wb_protocol_serialize_task_create(const char *operation_id,
                                                       const char *prompt,
                                                       char *output,
                                                       size_t output_capacity,
                                                       size_t *output_length);
wb_protocol_result_t wb_protocol_serialize_task_followup(const char *operation_id,
                                                         const char *task_id,
                                                         const char *text,
                                                         char *output,
                                                         size_t output_capacity,
                                                         size_t *output_length);

const char *wb_protocol_result_name(wb_protocol_result_t result);
