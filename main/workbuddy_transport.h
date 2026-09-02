#pragma once

#include "esp_err.h"
#include "workbuddy_types.h"

#include <stdbool.h>
#include <stddef.h>

typedef enum {
    WB_OPERATION_UNKNOWN = 0,
    WB_OPERATION_PENDING,
    WB_OPERATION_SUCCEEDED,
    WB_OPERATION_FAILED,
} wb_operation_status_t;

typedef struct {
    wb_operation_status_t status;
    char receipt_id[WB_ID_CAP];
    char error_code[WB_TITLE_CAP];
    bool retryable;
} wb_transport_result_t;

esp_err_t wb_transport_init(void);
esp_err_t wb_transport_fetch_snapshot(const char *after_cursor,
                                      wb_snapshot_t *snapshot);
esp_err_t wb_transport_submit_action(const wb_action_t *action,
                                     wb_transport_result_t *result);
esp_err_t wb_transport_get_operation(const char *operation_id,
                                     wb_transport_result_t *result);

/* A transcription upload owns the transport lock from begin through finish/abort. */
esp_err_t wb_transport_transcription_begin(const char *operation_id,
                                           size_t total_pcm_bytes);
esp_err_t wb_transport_transcription_write(const void *pcm, size_t bytes);
esp_err_t wb_transport_transcription_finish(char *transcript,
                                            size_t transcript_capacity);
void wb_transport_transcription_abort(void);
