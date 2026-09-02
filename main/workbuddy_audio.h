#pragma once

#include "esp_err.h"

#include <stdbool.h>

typedef enum {
    WB_AUDIO_READY = 0,
    WB_AUDIO_TRANSCRIBED,
    WB_AUDIO_CANCELLED,
    WB_AUDIO_FAILED,
} wb_audio_result_t;

typedef void (*wb_audio_result_callback_t)(wb_audio_result_t result,
                                           const char *operation_id,
                                           const char *text_or_error,
                                           void *context);

/* Initializes the blocking codec and its sole owner task. Live mode only. */
esp_err_t wb_audio_service_start(wb_audio_result_callback_t callback,
                                 void *context);
esp_err_t wb_audio_capture(const char *operation_id);
esp_err_t wb_audio_request_start(const char *operation_id);
esp_err_t wb_audio_request_finish(const char *operation_id);
esp_err_t wb_audio_request_cancel(const char *operation_id);
bool wb_audio_is_ready(void);
