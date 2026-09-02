#include "workbuddy_app.h"

#include "bsp_battery.h"
#include "bsp_display.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "workbuddy_audio.h"
#include "workbuddy_model.h"
#include "workbuddy_transport.h"
#include "workbuddy_ui.h"
#include "workbuddy_wifi.h"

#include <stdio.h>
#include <string.h>

#define WB_APP_EVENT_QUEUE_LENGTH 8U
#define WB_NETWORK_QUEUE_LENGTH 4U
#define WB_UI_TICK_MS 100U
#define WB_NOTICE_DURATION_MS 5000U

typedef enum {
    WB_APP_EVENT_INPUT = 0,
    WB_APP_EVENT_SNAPSHOT,
    WB_APP_EVENT_AUDIO_READY,
    WB_APP_EVENT_TRANSCRIPT,
    WB_APP_EVENT_AUDIO_CANCELLED,
    WB_APP_EVENT_AUDIO_ERROR,
    WB_APP_EVENT_ACTION_RESULT,
    WB_APP_EVENT_BATTERY,
} wb_app_event_type_t;

typedef struct {
    wb_app_event_type_t type;
    wb_input_t input;
    bool success;
    bool retryable;
    int value;
    char operation_id[WB_ID_CAP];
    char text[WB_TRANSCRIPT_CAP];
} wb_app_event_t;

typedef enum {
    WB_NETWORK_POLL = 0,
    WB_NETWORK_ACTION,
} wb_network_command_type_t;

typedef struct {
    wb_network_command_type_t type;
    wb_action_t action;
} wb_network_command_t;

static const char *TAG = "wb_app";
static QueueHandle_t s_events;

#if CONFIG_WB_DEMO_MODE
static wb_snapshot_t s_demo_snapshot;
#else
static SemaphoreHandle_t s_snapshot_lock;
static bool s_snapshot_pending;
static QueueHandle_t s_network_commands;
static wb_snapshot_t s_network_snapshot;
static volatile bool s_poll_queued;
#endif

static wb_model_t s_model;
static wb_ui_runtime_t s_runtime;
static lv_timer_t *s_ui_timer;
static bool s_battery_available;
static bool s_started;
static bool s_dirty;
static bool s_audio_start_pending;
#if !CONFIG_WB_DEMO_MODE
static char s_active_audio_operation[WB_ID_CAP];
#endif
static uint32_t s_notice_until_ms;
static char s_notice[WB_TITLE_CAP];

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000LL);
}

static void copy_text(char *destination, size_t capacity, const char *source)
{
    if (capacity == 0U) {
        return;
    }
    if (source == NULL) {
        destination[0] = '\0';
        return;
    }
    size_t length = strnlen(source, capacity - 1U);
    memcpy(destination, source, length);
    destination[length] = '\0';
}

static void set_notice(const char *notice)
{
    copy_text(s_notice, sizeof(s_notice), notice);
    s_notice_until_ms = now_ms() + WB_NOTICE_DURATION_MS;
    s_runtime.notice = s_notice;
    s_dirty = true;
}

static bool post_event(const wb_app_event_t *event)
{
    return s_events != NULL && xQueueSend(s_events, event, 0U) == pdTRUE;
}

#if !CONFIG_WB_DEMO_MODE
static bool post_critical_event(const wb_app_event_t *event)
{
    return s_events != NULL &&
           xQueueSend(s_events, event, portMAX_DELAY) == pdTRUE;
}
#endif

#if CONFIG_WB_DEMO_MODE
static void demo_snapshot(wb_snapshot_t *snapshot)
{
    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->version = 1U;
    snapshot->fresh = true;
    snapshot->assistant_available = true;
    snapshot->unread_count = 2U;
    snapshot->active_task_count = 2U;
    snapshot->message_count = 3U;
    snapshot->task_count = 3U;
    snapshot->output_count = 3U;
    (void)snprintf(snapshot->request_id, sizeof(snapshot->request_id), "demo-request-1");
    (void)snprintf(snapshot->cursor, sizeof(snapshot->cursor), "demo-cursor-1");

    (void)snprintf(snapshot->messages[0].id, sizeof(snapshot->messages[0].id),
                   "demo-message-1");
    (void)snprintf(snapshot->messages[0].title, sizeof(snapshot->messages[0].title),
                   "周报已完成");
    (void)snprintf(snapshot->messages[0].preview, sizeof(snapshot->messages[0].preview),
                   "已整理本周进展、风险与下周计划，请确认。 ");
    snapshot->messages[0].role = WB_MESSAGE_ROLE_ASSISTANT;
    snapshot->messages[0].unread = true;

    (void)snprintf(snapshot->messages[1].id, sizeof(snapshot->messages[1].id),
                   "demo-message-2");
    (void)snprintf(snapshot->messages[1].title, sizeof(snapshot->messages[1].title),
                   "数据分析有新发现");
    (void)snprintf(snapshot->messages[1].preview, sizeof(snapshot->messages[1].preview),
                   "转化率变化主要来自移动端入口。");
    snapshot->messages[1].role = WB_MESSAGE_ROLE_ASSISTANT;
    snapshot->messages[1].unread = true;

    (void)snprintf(snapshot->messages[2].id, sizeof(snapshot->messages[2].id),
                   "demo-message-3");
    (void)snprintf(snapshot->messages[2].title, sizeof(snapshot->messages[2].title),
                   "我的上一条指令");
    (void)snprintf(snapshot->messages[2].preview, sizeof(snapshot->messages[2].preview),
                   "把调研结果做成一页汇报。");
    snapshot->messages[2].role = WB_MESSAGE_ROLE_USER;

    (void)snprintf(snapshot->tasks[0].id, sizeof(snapshot->tasks[0].id), "demo-task-1");
    (void)snprintf(snapshot->tasks[0].title, sizeof(snapshot->tasks[0].title),
                   "竞品研究与策略建议");
    (void)snprintf(snapshot->tasks[0].preview, sizeof(snapshot->tasks[0].preview),
                   "正在交叉验证公开资料");
    snapshot->tasks[0].status = WB_TASK_STATUS_RUNNING;

    (void)snprintf(snapshot->tasks[1].id, sizeof(snapshot->tasks[1].id), "demo-task-2");
    (void)snprintf(snapshot->tasks[1].title, sizeof(snapshot->tasks[1].title),
                   "产品发布演示文稿");
    (void)snprintf(snapshot->tasks[1].preview, sizeof(snapshot->tasks[1].preview),
                   "需要确认封面方向");
    snapshot->tasks[1].status = WB_TASK_STATUS_NEEDS_INPUT;

    (void)snprintf(snapshot->tasks[2].id, sizeof(snapshot->tasks[2].id), "demo-task-3");
    (void)snprintf(snapshot->tasks[2].title, sizeof(snapshot->tasks[2].title),
                   "销售数据可视化");
    (void)snprintf(snapshot->tasks[2].preview, sizeof(snapshot->tasks[2].preview),
                   "图表和摘要已生成");
    snapshot->tasks[2].status = WB_TASK_STATUS_COMPLETED;

    (void)snprintf(snapshot->outputs[0].id, sizeof(snapshot->outputs[0].id),
                   "demo-output-1");
    (void)snprintf(snapshot->outputs[0].task_id, sizeof(snapshot->outputs[0].task_id),
                   "demo-task-3");
    (void)snprintf(snapshot->outputs[0].title, sizeof(snapshot->outputs[0].title),
                   "区域销售趋势图");
    (void)snprintf(snapshot->outputs[0].preview, sizeof(snapshot->outputs[0].preview),
                   "华东增长 18%%，华南增长 9%%，北区基本持平。");
    snapshot->outputs[0].kind = WB_OUTPUT_KIND_IMAGE;

    (void)snprintf(snapshot->outputs[1].id, sizeof(snapshot->outputs[1].id),
                   "demo-output-2");
    (void)snprintf(snapshot->outputs[1].task_id, sizeof(snapshot->outputs[1].task_id),
                   "demo-task-1");
    (void)snprintf(snapshot->outputs[1].title, sizeof(snapshot->outputs[1].title),
                   "研究计划");
    (void)snprintf(snapshot->outputs[1].preview, sizeof(snapshot->outputs[1].preview),
                   "范围、来源、对比维度和交付检查点。");
    snapshot->outputs[1].kind = WB_OUTPUT_KIND_PLAN;

    (void)snprintf(snapshot->outputs[2].id, sizeof(snapshot->outputs[2].id),
                   "demo-output-3");
    (void)snprintf(snapshot->outputs[2].task_id, sizeof(snapshot->outputs[2].task_id),
                   "demo-task-3");
    (void)snprintf(snapshot->outputs[2].title, sizeof(snapshot->outputs[2].title),
                   "分析摘要");
    (void)snprintf(snapshot->outputs[2].preview, sizeof(snapshot->outputs[2].preview),
                   "增长集中在三个高复购客户群，建议扩大定向触达。");
    snapshot->outputs[2].kind = WB_OUTPUT_KIND_OVERVIEW;
}

static const char *demo_transcript(wb_voice_context_t context)
{
    switch (context) {
    case WB_VOICE_REPLY:
        return "收到，请按这个版本继续。";
    case WB_VOICE_TASK_CREATE:
        return "整理本周项目进展并生成一页汇报。";
    case WB_VOICE_TASK_FOLLOWUP:
        return "请补充关键风险和下一步建议。";
    default:
        return "请继续。";
    }
}
#endif

#if !CONFIG_WB_DEMO_MODE
static void queue_poll(void)
{
    if (s_network_commands == NULL || s_poll_queued) {
        return;
    }
    wb_network_command_t command = {.type = WB_NETWORK_POLL};
    if (xQueueSend(s_network_commands, &command, 0U) == pdTRUE) {
        s_poll_queued = true;
    } else {
        set_notice("同步队列正忙");
    }
}
#endif

static void queue_action(void)
{
#if CONFIG_WB_DEMO_MODE
    char receipt[WB_ID_CAP];
    (void)snprintf(receipt, sizeof(receipt), "demo-%.58s", s_model.operation_id);
    (void)wb_model_complete_action(&s_model, true, receipt, false);
    set_notice("演示操作已完成");
#else
    wb_network_command_t command = {.type = WB_NETWORK_ACTION};
    if (!wb_model_pending_action(&s_model, &command.action) ||
        s_network_commands == NULL ||
        xQueueSend(s_network_commands, &command, 0U) != pdTRUE) {
        (void)wb_model_complete_action(&s_model, false, "device_queue_busy", true);
    }
#endif
}

static void apply_effect(wb_model_effect_t effect)
{
    switch (effect) {
    case WB_EFFECT_VOICE_STARTED:
#if CONFIG_WB_DEMO_MODE
        if (!wb_model_begin_recording(&s_model, s_model.operation_id, now_ms())) {
            set_notice("无法开始演示录音");
        }
#else
        if (!wb_wifi_is_connected()) {
            (void)wb_model_handle_input(&s_model, WB_INPUT_OK_LONG, now_ms());
            set_notice("Wi-Fi 或联网校时尚未就绪");
        } else {
            copy_text(s_active_audio_operation,
                      sizeof(s_active_audio_operation), s_model.operation_id);
            if (wb_audio_capture(s_active_audio_operation) == ESP_OK) {
                break;
            }
            s_active_audio_operation[0] = '\0';
            (void)wb_model_handle_input(&s_model, WB_INPUT_OK_LONG, now_ms());
            set_notice("麦克风或网关暂不可用");
        }
#endif
        break;
    case WB_EFFECT_VOICE_FINISHED:
#if CONFIG_WB_DEMO_MODE
        (void)wb_model_accept_transcript(&s_model,
                                         demo_transcript(s_model.voice_context));
#else
        if (wb_audio_request_finish(s_active_audio_operation) != ESP_OK) {
            set_notice("停止录音信号未送达，请稍候");
        }
#endif
        break;
    case WB_EFFECT_VOICE_CANCELLED:
#if !CONFIG_WB_DEMO_MODE
        if (s_active_audio_operation[0] != '\0') {
            (void)wb_audio_request_cancel(s_active_audio_operation);
        }
#endif
        break;
    case WB_EFFECT_SUBMIT_REQUESTED:
        queue_action();
        break;
    case WB_EFFECT_SYNC_REQUESTED:
#if CONFIG_WB_DEMO_MODE
        s_model.stale = false;
        set_notice("演示数据已同步");
#else
        queue_poll();
#endif
        break;
    case WB_EFFECT_NONE:
    default:
        break;
    }
    if (effect != WB_EFFECT_NONE) {
        s_dirty = true;
    }
}

#if !CONFIG_WB_DEMO_MODE
static void audio_callback(wb_audio_result_t result, const char *operation_id,
                           const char *text_or_error, void *context)
{
    (void)context;
    wb_app_event_t event = {0};
    if (result == WB_AUDIO_READY) {
        event.type = WB_APP_EVENT_AUDIO_READY;
    } else if (result == WB_AUDIO_TRANSCRIBED) {
        event.type = WB_APP_EVENT_TRANSCRIPT;
    } else if (result == WB_AUDIO_CANCELLED) {
        event.type = WB_APP_EVENT_AUDIO_CANCELLED;
    } else {
        event.type = WB_APP_EVENT_AUDIO_ERROR;
    }
    copy_text(event.operation_id, sizeof(event.operation_id), operation_id);
    copy_text(event.text, sizeof(event.text), text_or_error);
    if (!post_critical_event(&event)) {
        ESP_LOGE(TAG, "audio result could not be delivered");
    }
}

static void publish_action_result(const wb_transport_result_t *result,
                                  esp_err_t transport_error,
                                  const char *operation_id)
{
    wb_app_event_t event = {.type = WB_APP_EVENT_ACTION_RESULT};
    copy_text(event.operation_id, sizeof(event.operation_id), operation_id);
    if (transport_error == ESP_OK && result->status == WB_OPERATION_SUCCEEDED) {
        event.success = true;
        copy_text(event.text, sizeof(event.text), result->receipt_id);
    } else {
        event.success = false;
        event.retryable = result->status == WB_OPERATION_PENDING || result->retryable ||
                          transport_error == ESP_ERR_TIMEOUT;
        copy_text(event.text, sizeof(event.text),
                  result->error_code[0] != '\0' ? result->error_code : "delivery_unknown");
    }
    if (!post_critical_event(&event)) {
        ESP_LOGE(TAG, "action result could not be delivered");
    }
}

static void perform_action(const wb_action_t *action)
{
    wb_transport_result_t result = {0};
    esp_err_t error = wb_transport_submit_action(action, &result);
    if (error != ESP_OK && result.status == WB_OPERATION_UNKNOWN) {
        wb_transport_result_t receipt = {0};
        esp_err_t lookup = wb_transport_get_operation(action->operation_id, &receipt);
        if (lookup == ESP_OK || receipt.status != WB_OPERATION_UNKNOWN) {
            result = receipt;
            error = lookup;
        }
    }
    publish_action_result(&result, error, action->operation_id);
}

static void network_task(void *argument)
{
    (void)argument;
    char cursor[WB_ID_CAP] = {0};
    TickType_t poll_wait = 0U;
    for (;;) {
        wb_network_command_t command = {.type = WB_NETWORK_POLL};
        BaseType_t received = xQueueReceive(s_network_commands, &command, poll_wait);
        if (received != pdTRUE) {
            command.type = WB_NETWORK_POLL;
        }

        if (command.type == WB_NETWORK_ACTION) {
            perform_action(&command.action);
            poll_wait = 0U;
            continue;
        }

        s_poll_queued = false;
        if (!wb_wifi_wait_connected((uint32_t)CONFIG_WB_CONNECT_TIMEOUT_MS)) {
            const wb_app_event_t event = {.type = WB_APP_EVENT_SNAPSHOT};
            (void)post_event(&event);
        } else if (xSemaphoreTake(s_snapshot_lock, portMAX_DELAY) == pdTRUE) {
            esp_err_t fetch_error = wb_transport_fetch_snapshot(cursor, &s_network_snapshot);
            if (fetch_error == ESP_OK) {
                copy_text(cursor, sizeof(cursor), s_network_snapshot.cursor);
                s_snapshot_pending = true;
            }
            xSemaphoreGive(s_snapshot_lock);
            const wb_app_event_t event = {
                .type = WB_APP_EVENT_SNAPSHOT,
                .success = fetch_error == ESP_OK,
            };
            (void)post_event(&event);
        } else {
            const wb_app_event_t event = {.type = WB_APP_EVENT_SNAPSHOT};
            (void)post_event(&event);
        }
        poll_wait = pdMS_TO_TICKS((uint32_t)CONFIG_WB_POLL_INTERVAL_SECONDS * 1000U);
    }
}
#endif

static void battery_task(void *argument)
{
    (void)argument;
    for (;;) {
        wb_app_event_t event = {
            .type = WB_APP_EVENT_BATTERY,
            .value = bsp_battery_soc(),
        };
        (void)post_event(&event);
        vTaskDelay(pdMS_TO_TICKS(60000U));
    }
}

static void handle_event(const wb_app_event_t *event, uint32_t time_ms)
{
    switch (event->type) {
    case WB_APP_EVENT_INPUT:
#if !CONFIG_WB_DEMO_MODE
        if (event->input == WB_INPUT_OK_CLICK &&
            s_model.screen == WB_SCREEN_TASK_DETAIL) {
            set_notice("云任务追问将在后续版本开放");
            break;
        }
#endif
        apply_effect(wb_model_handle_input(&s_model, event->input, time_ms));
        s_dirty = true;
        break;
    case WB_APP_EVENT_SNAPSHOT:
#if !CONFIG_WB_DEMO_MODE
        if (xSemaphoreTake(s_snapshot_lock, 0U) == pdTRUE) {
            if (s_snapshot_pending) {
                bool advanced = wb_model_apply_snapshot(&s_model, &s_network_snapshot);
                s_snapshot_pending = false;
                if (advanced) {
                    set_notice("有新的 WorkBuddy 更新");
                }
            }
            if (event->success) {
                s_runtime.gateway_online = true;
            } else {
                wb_model_mark_stale(&s_model);
                s_runtime.gateway_online = false;
            }
            xSemaphoreGive(s_snapshot_lock);
        } else if (!post_event(event)) {
            ESP_LOGW(TAG, "snapshot result queue full");
        }
#endif
        s_dirty = true;
        break;
    case WB_APP_EVENT_AUDIO_READY:
#if !CONFIG_WB_DEMO_MODE
        if (strcmp(event->operation_id, s_active_audio_operation) == 0 &&
            strcmp(event->operation_id, s_model.operation_id) == 0 &&
            s_model.screen == WB_SCREEN_VOICE_PREPARING) {
            s_audio_start_pending = true;
            s_dirty = true;
        } else {
            (void)wb_audio_request_cancel(event->operation_id);
        }
#endif
        break;
    case WB_APP_EVENT_TRANSCRIPT:
#if !CONFIG_WB_DEMO_MODE
        if (strcmp(event->operation_id, s_active_audio_operation) == 0) {
            s_active_audio_operation[0] = '\0';
        }
#endif
        if (strcmp(event->operation_id, s_model.operation_id) == 0 &&
            wb_model_accept_transcript(&s_model, event->text)) {
            s_dirty = true;
        }
        break;
    case WB_APP_EVENT_AUDIO_CANCELLED:
#if !CONFIG_WB_DEMO_MODE
        if (strcmp(event->operation_id, s_active_audio_operation) == 0) {
            s_active_audio_operation[0] = '\0';
        }
        if (strcmp(event->operation_id, s_model.operation_id) == 0 &&
            (s_model.screen == WB_SCREEN_VOICE_PREPARING ||
             s_model.screen == WB_SCREEN_TRANSCRIBING ||
             s_model.screen == WB_SCREEN_RECORDING)) {
            (void)wb_model_handle_input(&s_model, WB_INPUT_OK_LONG, time_ms);
            s_audio_start_pending = false;
            set_notice("语音采集已取消");
        }
#endif
        break;
    case WB_APP_EVENT_AUDIO_ERROR:
#if !CONFIG_WB_DEMO_MODE
        if (strcmp(event->operation_id, s_active_audio_operation) == 0) {
            s_active_audio_operation[0] = '\0';
        }
#endif
        if (strcmp(event->operation_id, s_model.operation_id) == 0 &&
            (s_model.screen == WB_SCREEN_VOICE_PREPARING ||
             s_model.screen == WB_SCREEN_TRANSCRIBING ||
             s_model.screen == WB_SCREEN_RECORDING)) {
            apply_effect(wb_model_handle_input(&s_model, WB_INPUT_OK_LONG,
                                               time_ms));
            s_audio_start_pending = false;
            set_notice(event->text[0] != '\0' ? event->text : "语音转写失败");
        }
        break;
    case WB_APP_EVENT_ACTION_RESULT:
        if (strcmp(event->operation_id, s_model.operation_id) == 0) {
            (void)wb_model_complete_action(&s_model, event->success,
                                           event->text[0] != '\0'
                                               ? event->text : "gateway_error",
                                           event->retryable);
            s_dirty = true;
        }
        break;
    case WB_APP_EVENT_BATTERY:
        s_runtime.battery_percent = event->value;
        s_dirty = true;
        break;
    default:
        break;
    }
}

static void ui_timer_callback(lv_timer_t *timer)
{
    (void)timer;
    uint32_t time_ms = now_ms();
#if !CONFIG_WB_DEMO_MODE
    bool arm_pending_at_entry = s_audio_start_pending;
    UBaseType_t drain_limit = arm_pending_at_entry
        ? uxQueueMessagesWaiting(s_events)
        : WB_APP_EVENT_QUEUE_LENGTH;
#else
    const UBaseType_t drain_limit = WB_APP_EVENT_QUEUE_LENGTH;
#endif
    wb_app_event_t event;
    unsigned drained = 0U;
    while (drained < drain_limit &&
           xQueueReceive(s_events, &event, 0U) == pdTRUE) {
        handle_event(&event, time_ms);
        ++drained;
#if !CONFIG_WB_DEMO_MODE
        if (!arm_pending_at_entry && s_audio_start_pending) {
            break;
        }
#endif
    }
#if !CONFIG_WB_DEMO_MODE
    /* Inputs already queued while PREPARING are handled before this arm point. */
    if (arm_pending_at_entry && s_audio_start_pending) {
        if (wb_model_begin_recording(&s_model, s_active_audio_operation,
                                     time_ms)) {
            s_runtime.now_ms = time_ms;
            wb_ui_render(&s_model, &s_runtime);
            lv_refr_now(NULL);
            s_dirty = false;
            esp_err_t start_error = bsp_display_wait_idle();
            if (start_error == ESP_OK) {
                start_error = wb_audio_request_start(s_active_audio_operation);
            }
            if (start_error != ESP_OK) {
                (void)wb_audio_request_cancel(s_active_audio_operation);
                (void)wb_model_handle_input(&s_model, WB_INPUT_OK_LONG, time_ms);
                set_notice("无法启动语音采集");
            }
        } else {
            (void)wb_audio_request_cancel(s_active_audio_operation);
        }
        s_audio_start_pending = false;
    }
#endif
    apply_effect(wb_model_tick(&s_model, time_ms));

    s_runtime.now_ms = time_ms;
    s_runtime.wifi_label = wb_wifi_state_name(wb_wifi_state());
    s_runtime.audio_ready = wb_audio_is_ready();
    if (s_runtime.notice != NULL &&
        (int32_t)(time_ms - s_notice_until_ms) >= 0) {
        s_runtime.notice = NULL;
        s_dirty = true;
    }
    if (s_dirty || (s_model.screen == WB_SCREEN_RECORDING &&
                    (time_ms / 200U) != ((time_ms - WB_UI_TICK_MS) / 200U))) {
        wb_ui_render(&s_model, &s_runtime);
        s_dirty = false;
    }
}

esp_err_t wb_app_start(bool battery_available)
{
    if (s_started) {
        return ESP_OK;
    }
    s_events = xQueueCreate(WB_APP_EVENT_QUEUE_LENGTH, sizeof(wb_app_event_t));
    if (s_events == NULL) {
        return ESP_ERR_NO_MEM;
    }
    wb_model_init(&s_model, esp_random());
    memset(&s_runtime, 0, sizeof(s_runtime));
    s_runtime.demo_mode = WB_APP_DEMO_MODE;
    s_runtime.battery_percent = -1;
    s_runtime.wifi_label = wb_wifi_state_name(wb_wifi_state());
    s_battery_available = battery_available;

#if CONFIG_WB_DEMO_MODE
    demo_snapshot(&s_demo_snapshot);
    (void)wb_model_apply_snapshot(&s_model, &s_demo_snapshot);
    s_runtime.gateway_online = true;
#else
    s_snapshot_lock = xSemaphoreCreateMutex();
    s_network_commands = xQueueCreate(WB_NETWORK_QUEUE_LENGTH,
                                      sizeof(wb_network_command_t));
    if (s_snapshot_lock == NULL || s_network_commands == NULL) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t transport_error = wb_transport_init();
    esp_err_t wifi_error = wb_wifi_start();
    esp_err_t audio_error = ESP_ERR_INVALID_STATE;
    if (transport_error == ESP_OK && wifi_error == ESP_OK) {
        audio_error = wb_audio_service_start(audio_callback, NULL);
    }
    if (transport_error != ESP_OK) {
        set_notice("网关配置无效");
    } else if (wifi_error != ESP_OK) {
        set_notice("Wi-Fi 配置无效");
    } else if (audio_error != ESP_OK) {
        set_notice("麦克风不可用");
    }
    if (transport_error == ESP_OK && wifi_error == ESP_OK &&
        xTaskCreate(network_task, "wb_network", 8192U, NULL, 4U, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
#endif

    if (s_battery_available &&
        xTaskCreate(battery_task, "wb_battery", 2048U, NULL, 2U, NULL) != pdPASS) {
        ESP_LOGW(TAG, "battery worker unavailable");
        s_battery_available = false;
    }

    if (!wb_ui_init()) {
        return ESP_FAIL;
    }
    s_dirty = true;
    s_ui_timer = lv_timer_create(ui_timer_callback, WB_UI_TICK_MS, NULL);
    if (s_ui_timer == NULL) {
        return ESP_ERR_NO_MEM;
    }
    s_started = true;
    wb_ui_render(&s_model, &s_runtime);
    return ESP_OK;
}

void wb_app_button_callback(bsp_btn_t button, bsp_btn_ev_t event, void *context)
{
    (void)context;
    wb_app_event_t app_event = {.type = WB_APP_EVENT_INPUT};
    if (event == BSP_BTN_CLICK) {
        if (button == BSP_BTN_UP) {
            app_event.input = WB_INPUT_UP_CLICK;
        } else if (button == BSP_BTN_DOWN) {
            app_event.input = WB_INPUT_DOWN_CLICK;
        } else if (button == BSP_BTN_OK) {
            app_event.input = WB_INPUT_OK_CLICK;
        } else {
            return;
        }
    } else if (event == BSP_BTN_LONG && button == BSP_BTN_OK) {
        app_event.input = WB_INPUT_OK_LONG;
    } else {
        return;
    }
    if (!post_event(&app_event)) {
        ESP_LOGW(TAG, "button event queue full");
    }
}
