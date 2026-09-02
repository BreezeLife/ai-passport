#include "workbuddy_wifi.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"

#include <string.h>
#include <time.h>

#define WB_WIFI_CONNECTED_BIT BIT0
#define WB_WIFI_HAS_IP_BIT BIT1
#define WB_WIFI_SYNC_TIME_BIT BIT2
#define WB_WIFI_MAX_BACKOFF_MS 30000U
#define WB_WIFI_TIME_RETRY_MS 5000U
#define WB_WIFI_MIN_VALID_EPOCH INT64_C(1704067200)

static volatile wb_wifi_state_t s_state = WB_WIFI_DISABLED;

#if !CONFIG_WB_DEMO_MODE
static const char *TAG = "wb_wifi";
static EventGroupHandle_t s_events;
static SemaphoreHandle_t s_state_lock;
static esp_timer_handle_t s_reconnect_timer;
static esp_event_handler_instance_t s_wifi_handler;
static esp_event_handler_instance_t s_ip_handler;
static uint32_t s_retry_count;
static bool s_started;
static bool s_time_sync_required;

/* s_state_lock must be held. */
static void schedule_reconnect_locked(void);

static bool system_time_is_valid(void)
{
    time_t now = 0;
    (void)time(&now);
    return (int64_t)now >= WB_WIFI_MIN_VALID_EPOCH;
}

/* s_state_lock must be held. */
static void set_connected_if_current_locked(void)
{
    EventBits_t bits = xEventGroupGetBits(s_events);
    if (!s_time_sync_required && (bits & WB_WIFI_HAS_IP_BIT) != 0U) {
        s_retry_count = 0U;
        s_state = WB_WIFI_CONNECTED;
        xEventGroupSetBits(s_events, WB_WIFI_CONNECTED_BIT);
        ESP_LOGI(TAG, "station network prerequisites are satisfied");
    }
}

static void time_sync_task(void *argument)
{
    (void)argument;
    for (;;) {
        (void)xEventGroupWaitBits(s_events, WB_WIFI_SYNC_TIME_BIT,
                                  pdTRUE, pdTRUE, portMAX_DELAY);
        if (xSemaphoreTake(s_state_lock, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        bool should_sync = s_time_sync_required &&
            (xEventGroupGetBits(s_events) & WB_WIFI_HAS_IP_BIT) != 0U;
        if (should_sync) {
            s_state = WB_WIFI_SYNCING_TIME;
        }
        xSemaphoreGive(s_state_lock);

        while (should_sync) {
            esp_err_t sync_error = esp_netif_sntp_sync_wait(
                pdMS_TO_TICKS(CONFIG_WB_TIME_SYNC_TIMEOUT_MS));
            bool time_is_valid = system_time_is_valid();
            if (sync_error == ESP_OK && time_is_valid) {
                if (xSemaphoreTake(s_state_lock, portMAX_DELAY) == pdTRUE) {
                    if (s_time_sync_required &&
                        (xEventGroupGetBits(s_events) & WB_WIFI_HAS_IP_BIT) != 0U) {
                        s_time_sync_required = false;
                        xEventGroupClearBits(s_events, WB_WIFI_SYNC_TIME_BIT);
                        set_connected_if_current_locked();
                    }
                    xSemaphoreGive(s_state_lock);
                }
                break;
            }
            if (sync_error == ESP_OK && !time_is_valid) {
                esp_err_t restart_error = esp_netif_sntp_start();
                if (restart_error != ESP_OK) {
                    ESP_LOGW(TAG, "SNTP restart failed: %s",
                             esp_err_to_name(restart_error));
                }
            }
            ESP_LOGW(TAG, "time sync pending; HTTPS remains disabled");
            vTaskDelay(pdMS_TO_TICKS(WB_WIFI_TIME_RETRY_MS));
            if (xSemaphoreTake(s_state_lock, portMAX_DELAY) == pdTRUE) {
                should_sync = s_time_sync_required &&
                    (xEventGroupGetBits(s_events) & WB_WIFI_HAS_IP_BIT) != 0U;
                xSemaphoreGive(s_state_lock);
            } else {
                should_sync = false;
            }
        }
    }
}

static uint32_t reconnect_delay_ms(void)
{
    uint32_t shift = s_retry_count < 5U ? s_retry_count : 5U;
    uint32_t delay = 1000U << shift;
    return delay < WB_WIFI_MAX_BACKOFF_MS ? delay : WB_WIFI_MAX_BACKOFF_MS;
}

static void reconnect_timer_callback(void *argument)
{
    (void)argument;
    if (xSemaphoreTake(s_state_lock, portMAX_DELAY) != pdTRUE) {
        return;
    }
    if (!s_started ||
        (xEventGroupGetBits(s_events) & WB_WIFI_CONNECTED_BIT) != 0U) {
        xSemaphoreGive(s_state_lock);
        return;
    }
    s_state = WB_WIFI_CONNECTING;
    xSemaphoreGive(s_state_lock);
    esp_err_t error = esp_wifi_connect();
    if (error != ESP_OK) {
        if (xSemaphoreTake(s_state_lock, portMAX_DELAY) == pdTRUE) {
            if (s_started &&
                (xEventGroupGetBits(s_events) & WB_WIFI_CONNECTED_BIT) == 0U) {
                schedule_reconnect_locked();
            }
            xSemaphoreGive(s_state_lock);
        }
        ESP_LOGW(TAG, "connect start failed: %s", esp_err_to_name(error));
    }
}

/* s_state_lock must be held. */
static void schedule_reconnect_locked(void)
{
    uint32_t delay_ms = reconnect_delay_ms();
    ++s_retry_count;
    s_state = WB_WIFI_BACKOFF;
    if (s_reconnect_timer != NULL) {
        (void)esp_timer_stop(s_reconnect_timer);
        esp_err_t error = esp_timer_start_once(s_reconnect_timer,
                                                (uint64_t)delay_ms * 1000ULL);
        if (error != ESP_OK) {
            s_state = WB_WIFI_FAILED;
            ESP_LOGW(TAG, "reconnect scheduling failed: %s", esp_err_to_name(error));
        }
    }
}

static void wifi_event_handler(void *argument, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    (void)argument;
    (void)event_base;
    (void)event_data;

    if (event_id == WIFI_EVENT_STA_START) {
        if (xSemaphoreTake(s_state_lock, portMAX_DELAY) != pdTRUE) {
            return;
        }
        s_state = WB_WIFI_CONNECTING;
        xSemaphoreGive(s_state_lock);
        if (esp_wifi_connect() != ESP_OK) {
            if (xSemaphoreTake(s_state_lock, portMAX_DELAY) == pdTRUE) {
                schedule_reconnect_locked();
                xSemaphoreGive(s_state_lock);
            }
        }
    } else if (event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (xSemaphoreTake(s_state_lock, portMAX_DELAY) != pdTRUE) {
            return;
        }
        xEventGroupClearBits(s_events, WB_WIFI_CONNECTED_BIT |
                                       WB_WIFI_HAS_IP_BIT |
                                       WB_WIFI_SYNC_TIME_BIT);
        schedule_reconnect_locked();
        xSemaphoreGive(s_state_lock);
    }
}

static void ip_event_handler(void *argument, esp_event_base_t event_base,
                             int32_t event_id, void *event_data)
{
    (void)argument;
    (void)event_base;
    (void)event_data;

    if (event_id == IP_EVENT_STA_GOT_IP) {
        if (xSemaphoreTake(s_state_lock, portMAX_DELAY) != pdTRUE) {
            return;
        }
        s_retry_count = 0U;
        xEventGroupSetBits(s_events, WB_WIFI_HAS_IP_BIT);
        if (s_time_sync_required) {
            s_state = WB_WIFI_SYNCING_TIME;
            xEventGroupSetBits(s_events, WB_WIFI_SYNC_TIME_BIT);
            ESP_LOGI(TAG, "station has an IP address; validating wall-clock time");
        } else {
            set_connected_if_current_locked();
        }
        xSemaphoreGive(s_state_lock);
    }
}

static esp_err_t copy_credentials(wifi_config_t *configuration)
{
    const char *ssid = CONFIG_WB_WIFI_SSID;
    const char *password = CONFIG_WB_WIFI_PASSWORD;
    size_t ssid_length = strlen(ssid);
    size_t password_length = strlen(password);

    if (ssid_length == 0U || ssid_length >= sizeof(configuration->sta.ssid) ||
        password_length >= sizeof(configuration->sta.password)) {
        return ESP_ERR_INVALID_ARG;
    }
    memcpy(configuration->sta.ssid, ssid, ssid_length + 1U);
    memcpy(configuration->sta.password, password, password_length + 1U);
    configuration->sta.threshold.authmode = password_length == 0U
        ? WIFI_AUTH_OPEN
        : WIFI_AUTH_WPA2_PSK;
    configuration->sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
    return ESP_OK;
}
#endif

esp_err_t wb_wifi_start(void)
{
#if CONFIG_WB_DEMO_MODE
    s_state = WB_WIFI_DISABLED;
    return ESP_OK;
#else
    if (s_started) {
        return ESP_OK;
    }

    wifi_config_t configuration = {0};
    esp_err_t error = copy_credentials(&configuration);
    if (error != ESP_OK) {
        s_state = WB_WIFI_FAILED;
        ESP_LOGE(TAG, "Wi-Fi configuration is missing or exceeds station limits");
        return error;
    }

    s_events = xEventGroupCreate();
    s_state_lock = xSemaphoreCreateMutex();
    if (s_events == NULL || s_state_lock == NULL) {
        s_state = WB_WIFI_FAILED;
        return ESP_ERR_NO_MEM;
    }
    s_state = WB_WIFI_STARTING;

    error = esp_netif_init();
    if (error != ESP_OK && error != ESP_ERR_INVALID_STATE) {
        s_state = WB_WIFI_FAILED;
        return error;
    }
    error = esp_event_loop_create_default();
    if (error != ESP_OK && error != ESP_ERR_INVALID_STATE) {
        s_state = WB_WIFI_FAILED;
        return error;
    }
    if (esp_netif_create_default_wifi_sta() == NULL) {
        s_state = WB_WIFI_FAILED;
        return ESP_FAIL;
    }

    s_time_sync_required = strncmp(CONFIG_WB_GATEWAY_URL, "https://", 8U) == 0;
    if (s_time_sync_required) {
        esp_sntp_config_t sntp = ESP_NETIF_SNTP_DEFAULT_CONFIG(CONFIG_WB_SNTP_SERVER);
        error = esp_netif_sntp_init(&sntp);
        if (error != ESP_OK ||
            xTaskCreate(time_sync_task, "wb_time", 3072U, NULL, 3U, NULL) != pdPASS) {
            s_state = WB_WIFI_FAILED;
            return error != ESP_OK ? error : ESP_ERR_NO_MEM;
        }
    }

    wifi_init_config_t initial = WIFI_INIT_CONFIG_DEFAULT();
    initial.nvs_enable = 0;
    if ((error = esp_wifi_init(&initial)) != ESP_OK ||
        (error = esp_wifi_set_storage(WIFI_STORAGE_RAM)) != ESP_OK ||
        (error = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                     wifi_event_handler, NULL,
                                                     &s_wifi_handler)) != ESP_OK ||
        (error = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                     ip_event_handler, NULL,
                                                     &s_ip_handler)) != ESP_OK) {
        s_state = WB_WIFI_FAILED;
        return error;
    }

    const esp_timer_create_args_t timer_args = {
        .callback = reconnect_timer_callback,
        .arg = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "wb_wifi_retry",
        .skip_unhandled_events = true,
    };
    if ((error = esp_timer_create(&timer_args, &s_reconnect_timer)) != ESP_OK ||
        (error = esp_wifi_set_mode(WIFI_MODE_STA)) != ESP_OK ||
        (error = esp_wifi_set_config(WIFI_IF_STA, &configuration)) != ESP_OK) {
        s_state = WB_WIFI_FAILED;
        return error;
    }

    s_started = true;
    if ((error = esp_wifi_start()) != ESP_OK) {
        s_started = false;
        s_state = WB_WIFI_FAILED;
        return error;
    }
    return ESP_OK;
#endif
}

bool wb_wifi_is_connected(void)
{
#if CONFIG_WB_DEMO_MODE
    return false;
#else
    return s_events != NULL &&
        (xEventGroupGetBits(s_events) & WB_WIFI_CONNECTED_BIT) != 0U;
#endif
}

bool wb_wifi_wait_connected(uint32_t timeout_ms)
{
#if CONFIG_WB_DEMO_MODE
    (void)timeout_ms;
    return false;
#else
    if (s_events == NULL) {
        return false;
    }
    EventBits_t bits = xEventGroupWaitBits(s_events, WB_WIFI_CONNECTED_BIT,
                                           pdFALSE, pdTRUE,
                                           pdMS_TO_TICKS(timeout_ms));
    return (bits & WB_WIFI_CONNECTED_BIT) != 0U;
#endif
}

wb_wifi_state_t wb_wifi_state(void)
{
#if !CONFIG_WB_DEMO_MODE
    if (s_state_lock != NULL &&
        xSemaphoreTake(s_state_lock, pdMS_TO_TICKS(20U)) == pdTRUE) {
        wb_wifi_state_t state = s_state;
        xSemaphoreGive(s_state_lock);
        return state;
    }
#endif
    return s_state;
}

const char *wb_wifi_state_name(wb_wifi_state_t state)
{
    switch (state) {
    case WB_WIFI_DISABLED: return "DEMO";
    case WB_WIFI_STARTING: return "STARTING";
    case WB_WIFI_CONNECTING: return "CONNECTING";
    case WB_WIFI_SYNCING_TIME: return "TIME SYNC";
    case WB_WIFI_CONNECTED: return "ONLINE";
    case WB_WIFI_BACKOFF: return "RETRYING";
    case WB_WIFI_FAILED: return "FAILED";
    default: return "UNKNOWN";
    }
}
