#include "workbuddy_audio.h"

#include "bsp_audio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "workbuddy_transport.h"
#include "workbuddy_types.h"

#include <stdint.h>
#include <stdatomic.h>
#include <string.h>

#define WB_AUDIO_CHUNK_BYTES 2048U
#define WB_AUDIO_TOTAL_BYTES (16000U * 2U * 5U)
#define WB_AUDIO_CONTROL_QUEUE_LENGTH 8U
#define WB_AUDIO_CONTROL_START BIT0
#define WB_AUDIO_CONTROL_FINISH BIT1
#define WB_AUDIO_CONTROL_CANCEL BIT2

static volatile bool s_ready;

#if !CONFIG_WB_DEMO_MODE
typedef struct {
    char operation_id[WB_ID_CAP];
} wb_audio_command_t;

typedef struct {
    uint32_t type;
    char operation_id[WB_ID_CAP];
} wb_audio_control_t;

static const char *TAG = "wb_audio";
static QueueHandle_t s_commands;
static QueueHandle_t s_controls;
static TaskHandle_t s_task;
static wb_audio_result_callback_t s_callback;
static void *s_callback_context;
static atomic_bool s_recording = ATOMIC_VAR_INIT(false);

static void publish(wb_audio_result_t result, const char *operation_id,
                    const char *text)
{
    if (s_callback != NULL) {
        s_callback(result, operation_id, text, s_callback_context);
    }
}

static uint32_t receive_controls(const char *operation_id,
                                 uint32_t accumulated,
                                 TickType_t first_wait)
{
    wb_audio_control_t control;
    TickType_t wait = first_wait;
    while (xQueueReceive(s_controls, &control, wait) == pdTRUE) {
        if (strcmp(control.operation_id, operation_id) == 0) {
            accumulated |= control.type;
        }
        wait = 0U;
    }
    return accumulated;
}

static esp_err_t send_control(uint32_t type, const char *operation_id)
{
    if (!s_ready || s_controls == NULL || operation_id == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    size_t operation_length = strnlen(operation_id, WB_ID_CAP);
    if (operation_length == 0U || operation_length >= WB_ID_CAP) {
        return ESP_ERR_INVALID_STATE;
    }
    wb_audio_control_t control = {.type = type};
    memcpy(control.operation_id, operation_id, operation_length + 1U);
    return xQueueSend(s_controls, &control, pdMS_TO_TICKS(20U)) == pdTRUE
        ? ESP_OK
        : ESP_ERR_TIMEOUT;
}

static esp_err_t write_zeros(size_t bytes)
{
    static const uint8_t zeros[WB_AUDIO_CHUNK_BYTES] = {0};
    size_t written = 0U;
    while (written < bytes) {
        size_t chunk = bytes - written;
        if (chunk > sizeof(zeros)) {
            chunk = sizeof(zeros);
        }
        esp_err_t error = wb_transport_transcription_write(zeros, chunk);
        if (error != ESP_OK) {
            return error;
        }
        written += chunk;
    }
    return ESP_OK;
}

static void capture_once(const wb_audio_command_t *command)
{
    uint8_t pcm[WB_AUDIO_CHUNK_BYTES];
    size_t uploaded = 0U;
    uint32_t controls = 0U;
    char transcript[WB_TRANSCRIPT_CAP];

    esp_err_t error = wb_transport_transcription_begin(command->operation_id,
                                                        WB_AUDIO_TOTAL_BYTES);
    if (error != ESP_OK) {
        controls = receive_controls(command->operation_id, controls, 0U);
        atomic_store_explicit(&s_recording, false, memory_order_release);
        publish((controls & WB_AUDIO_CONTROL_CANCEL) != 0U
                    ? WB_AUDIO_CANCELLED : WB_AUDIO_FAILED,
                command->operation_id,
                (controls & WB_AUDIO_CONTROL_CANCEL) != 0U
                    ? "cancelled" : "gateway_unavailable");
        return;
    }

    publish(WB_AUDIO_READY, command->operation_id, "");
    while ((controls & (WB_AUDIO_CONTROL_START | WB_AUDIO_CONTROL_CANCEL)) == 0U) {
        controls = receive_controls(command->operation_id, controls, portMAX_DELAY);
    }
    if ((controls & WB_AUDIO_CONTROL_CANCEL) != 0U) {
        wb_transport_transcription_abort();
        atomic_store_explicit(&s_recording, false, memory_order_release);
        publish(WB_AUDIO_CANCELLED, command->operation_id, "cancelled");
        return;
    }

    error = bsp_audio_flush_input();
    if (error != ESP_OK) {
        wb_transport_transcription_abort();
        controls = receive_controls(command->operation_id, controls, 0U);
        atomic_store_explicit(&s_recording, false, memory_order_release);
        publish((controls & WB_AUDIO_CONTROL_CANCEL) != 0U
                    ? WB_AUDIO_CANCELLED : WB_AUDIO_FAILED,
                command->operation_id,
                (controls & WB_AUDIO_CONTROL_CANCEL) != 0U
                    ? "cancelled" : "audio_start_failed");
        return;
    }

    while (uploaded < WB_AUDIO_TOTAL_BYTES) {
        controls = receive_controls(command->operation_id, controls, 0U);
        if ((controls & WB_AUDIO_CONTROL_CANCEL) != 0U) {
            wb_transport_transcription_abort();
            atomic_store_explicit(&s_recording, false, memory_order_release);
            publish(WB_AUDIO_CANCELLED, command->operation_id, "cancelled");
            return;
        }
        if ((controls & WB_AUDIO_CONTROL_FINISH) != 0U) {
            error = write_zeros(WB_AUDIO_TOTAL_BYTES - uploaded);
            uploaded = WB_AUDIO_TOTAL_BYTES;
            break;
        }

        size_t chunk = WB_AUDIO_TOTAL_BYTES - uploaded;
        if (chunk > sizeof(pcm)) {
            chunk = sizeof(pcm);
        }
        error = bsp_audio_read(pcm, chunk);
        if (error == ESP_OK) {
            error = wb_transport_transcription_write(pcm, chunk);
        }
        if (error != ESP_OK) {
            wb_transport_transcription_abort();
            controls = receive_controls(command->operation_id, controls, 0U);
            atomic_store_explicit(&s_recording, false, memory_order_release);
            publish((controls & WB_AUDIO_CONTROL_CANCEL) != 0U
                        ? WB_AUDIO_CANCELLED : WB_AUDIO_FAILED,
                    command->operation_id,
                    (controls & WB_AUDIO_CONTROL_CANCEL) != 0U
                        ? "cancelled" : "audio_upload_failed");
            return;
        }
        uploaded += chunk;
    }

    controls = receive_controls(command->operation_id, controls, 0U);
    if ((controls & WB_AUDIO_CONTROL_CANCEL) != 0U) {
        wb_transport_transcription_abort();
        atomic_store_explicit(&s_recording, false, memory_order_release);
        publish(WB_AUDIO_CANCELLED, command->operation_id, "cancelled");
        return;
    }
    if (error == ESP_OK) {
        error = wb_transport_transcription_finish(transcript, sizeof(transcript));
    } else {
        wb_transport_transcription_abort();
    }
    controls = receive_controls(command->operation_id, controls, 0U);
    atomic_store_explicit(&s_recording, false, memory_order_release);
    if ((controls & WB_AUDIO_CONTROL_CANCEL) != 0U) {
        publish(WB_AUDIO_CANCELLED, command->operation_id, "cancelled");
    } else if (error == ESP_OK) {
        publish(WB_AUDIO_TRANSCRIBED, command->operation_id, transcript);
    } else {
        publish(WB_AUDIO_FAILED, command->operation_id, "transcription_failed");
    }
}

static void audio_task(void *argument)
{
    (void)argument;
    wb_audio_command_t command;
    for (;;) {
        if (xQueueReceive(s_commands, &command, portMAX_DELAY) == pdTRUE) {
            capture_once(&command);
        }
    }
}
#endif

esp_err_t wb_audio_service_start(wb_audio_result_callback_t callback,
                                 void *context)
{
#if CONFIG_WB_DEMO_MODE
    (void)callback;
    (void)context;
    s_ready = false;
    return ESP_OK;
#else
    if (s_ready) {
        return ESP_OK;
    }
    s_callback = callback;
    s_callback_context = context;
    esp_err_t error = bsp_audio_init();
    if (error == ESP_OK) {
        error = bsp_audio_set_format(16000U, 16U, 1U);
    }
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "microphone initialization failed: %s", esp_err_to_name(error));
        return error;
    }
    s_commands = xQueueCreate(1U, sizeof(wb_audio_command_t));
    s_controls = xQueueCreate(WB_AUDIO_CONTROL_QUEUE_LENGTH,
                              sizeof(wb_audio_control_t));
    if (s_commands == NULL || s_controls == NULL) {
        if (s_commands != NULL) {
            vQueueDelete(s_commands);
            s_commands = NULL;
        }
        if (s_controls != NULL) {
            vQueueDelete(s_controls);
            s_controls = NULL;
        }
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(audio_task, "wb_audio", 6144U, NULL, 5U, &s_task) != pdPASS) {
        vQueueDelete(s_commands);
        vQueueDelete(s_controls);
        s_commands = NULL;
        s_controls = NULL;
        return ESP_ERR_NO_MEM;
    }
    s_ready = true;
    return ESP_OK;
#endif
}

esp_err_t wb_audio_capture(const char *operation_id)
{
#if CONFIG_WB_DEMO_MODE
    (void)operation_id;
    return ESP_ERR_INVALID_STATE;
#else
    if (!s_ready || operation_id == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    size_t operation_length = strnlen(operation_id, WB_ID_CAP);
    if (operation_length == 0U || operation_length >= WB_ID_CAP) {
        return ESP_ERR_INVALID_STATE;
    }
    bool expected = false;
    if (!atomic_compare_exchange_strong_explicit(
            &s_recording, &expected, true,
            memory_order_acq_rel, memory_order_acquire)) {
        return ESP_ERR_INVALID_STATE;
    }
    wb_audio_command_t command = {0};
    memcpy(command.operation_id, operation_id, operation_length + 1U);
    xQueueReset(s_controls);
    if (xQueueSend(s_commands, &command, 0U) != pdTRUE) {
        atomic_store_explicit(&s_recording, false, memory_order_release);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
#endif
}

esp_err_t wb_audio_request_start(const char *operation_id)
{
#if CONFIG_WB_DEMO_MODE
    (void)operation_id;
    return ESP_ERR_INVALID_STATE;
#else
    return send_control(WB_AUDIO_CONTROL_START, operation_id);
#endif
}

esp_err_t wb_audio_request_finish(const char *operation_id)
{
#if CONFIG_WB_DEMO_MODE
    (void)operation_id;
    return ESP_ERR_INVALID_STATE;
#else
    return send_control(WB_AUDIO_CONTROL_FINISH, operation_id);
#endif
}

esp_err_t wb_audio_request_cancel(const char *operation_id)
{
#if CONFIG_WB_DEMO_MODE
    (void)operation_id;
    return ESP_ERR_INVALID_STATE;
#else
    return send_control(WB_AUDIO_CONTROL_CANCEL, operation_id);
#endif
}

bool wb_audio_is_ready(void)
{
    return s_ready;
}
