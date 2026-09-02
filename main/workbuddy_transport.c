#include "workbuddy_transport.h"

#include "workbuddy_protocol.h"

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#ifndef CONFIG_WB_GATEWAY_URL
#define CONFIG_WB_GATEWAY_URL ""
#endif

#ifndef CONFIG_WB_DEVICE_TOKEN
#define CONFIG_WB_DEVICE_TOKEN ""
#endif

#define WB_URL_CAP 640U
#define WB_AUTH_CAP 384U
#define WB_ACTION_BODY_CAP 4096U
#define WB_SMALL_RESPONSE_CAP 2048U
#define WB_PCM_BYTES_PER_SECOND (16000U * 2U)
#define WB_PCM_MAX_BYTES (WB_PCM_BYTES_PER_SECOND * 5U)

static const char *TAG = "wb_transport";

static SemaphoreHandle_t s_lock;
static SemaphoreHandle_t s_snapshot_lock;
static char s_snapshot_body[WB_SNAPSHOT_MAX_BYTES + 1U];
static char s_small_body[WB_SMALL_RESPONSE_CAP + 1U];
static char s_action_body[WB_ACTION_BODY_CAP];

static esp_http_client_handle_t s_upload;
static size_t s_upload_expected;
static size_t s_upload_written;
static bool s_upload_owns_lock;

#if !CONFIG_WB_DEMO_MODE
static bool starts_with(const char *text, const char *prefix)
{
    return strncmp(text, prefix, strlen(prefix)) == 0;
}

static bool transport_configuration_valid(void)
{
    const char *url = CONFIG_WB_GATEWAY_URL;
    size_t length = strlen(url);
    if (length == 0U || length >= WB_URL_CAP ||
        strlen(CONFIG_WB_DEVICE_TOKEN) == 0U ||
        strlen(CONFIG_WB_DEVICE_TOKEN) + strlen("Bearer ") >= WB_AUTH_CAP ||
        url[length - 1U] == '/') {
        return false;
    }
    if (starts_with(url, "https://")) {
        return true;
    }
#if CONFIG_WB_ALLOW_INSECURE_HTTP
    return starts_with(url, "http://");
#else
    return false;
#endif
}
#endif

static bool utf8_valid(const char *text)
{
    const unsigned char *cursor = (const unsigned char *)text;
    while (*cursor != 0U) {
        if (*cursor < 0x80U) {
            ++cursor;
        } else if (*cursor >= 0xC2U && *cursor <= 0xDFU &&
                   (cursor[1] & 0xC0U) == 0x80U) {
            cursor += 2;
        } else if (*cursor == 0xE0U && cursor[1] >= 0xA0U && cursor[1] <= 0xBFU &&
                   (cursor[2] & 0xC0U) == 0x80U) {
            cursor += 3;
        } else if (((*cursor >= 0xE1U && *cursor <= 0xECU) ||
                    (*cursor >= 0xEEU && *cursor <= 0xEFU)) &&
                   (cursor[1] & 0xC0U) == 0x80U &&
                   (cursor[2] & 0xC0U) == 0x80U) {
            cursor += 3;
        } else if (*cursor == 0xEDU && cursor[1] >= 0x80U && cursor[1] <= 0x9FU &&
                   (cursor[2] & 0xC0U) == 0x80U) {
            cursor += 3;
        } else if (*cursor == 0xF0U && cursor[1] >= 0x90U && cursor[1] <= 0xBFU &&
                   (cursor[2] & 0xC0U) == 0x80U &&
                   (cursor[3] & 0xC0U) == 0x80U) {
            cursor += 4;
        } else if (*cursor >= 0xF1U && *cursor <= 0xF3U &&
                   (cursor[1] & 0xC0U) == 0x80U &&
                   (cursor[2] & 0xC0U) == 0x80U &&
                   (cursor[3] & 0xC0U) == 0x80U) {
            cursor += 4;
        } else if (*cursor == 0xF4U && cursor[1] >= 0x80U && cursor[1] <= 0x8FU &&
                   (cursor[2] & 0xC0U) == 0x80U &&
                   (cursor[3] & 0xC0U) == 0x80U) {
            cursor += 4;
        } else {
            return false;
        }
    }
    return true;
}

static bool append_encoded(char *output, size_t capacity, size_t *used,
                           const char *input)
{
    static const char HEX[] = "0123456789ABCDEF";
    const unsigned char *cursor = (const unsigned char *)input;
    while (*cursor != 0U) {
        unsigned char value = *cursor++;
        bool unreserved = isalnum(value) != 0 || value == '-' || value == '_' ||
                          value == '.' || value == '~';
        size_t required = unreserved ? 1U : 3U;
        if (*used + required >= capacity) {
            return false;
        }
        if (unreserved) {
            output[(*used)++] = (char)value;
        } else {
            output[(*used)++] = '%';
            output[(*used)++] = HEX[value >> 4U];
            output[(*used)++] = HEX[value & 0x0FU];
        }
    }
    output[*used] = '\0';
    return true;
}

static bool make_url(char *output, size_t capacity, const char *path,
                     const char *query_name, const char *query_value)
{
    int written = snprintf(output, capacity, "%s%s", CONFIG_WB_GATEWAY_URL, path);
    if (written < 0 || (size_t)written >= capacity) {
        return false;
    }
    size_t used = (size_t)written;
    if (query_name == NULL || query_value == NULL || query_value[0] == '\0') {
        return true;
    }
    written = snprintf(&output[used], capacity - used, "?%s=", query_name);
    if (written < 0 || (size_t)written >= capacity - used) {
        return false;
    }
    used += (size_t)written;
    return append_encoded(output, capacity, &used, query_value);
}

static bool make_operation_url(char *output, size_t capacity,
                               const char *operation_id)
{
    int written = snprintf(output, capacity, "%s/v1/operations/",
                           CONFIG_WB_GATEWAY_URL);
    if (written < 0 || (size_t)written >= capacity) {
        return false;
    }
    size_t used = (size_t)written;
    return append_encoded(output, capacity, &used, operation_id);
}

static esp_http_client_handle_t make_client(const char *url,
                                            esp_http_client_method_t method)
{
    const esp_http_client_config_t configuration = {
        .url = url,
        .method = method,
        .timeout_ms = CONFIG_WB_HTTP_TIMEOUT_MS,
        .buffer_size = 1024,
        .buffer_size_tx = 1024,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .disable_auto_redirect = true,
        .keep_alive_enable = false,
    };
    esp_http_client_handle_t client = esp_http_client_init(&configuration);
    if (client == NULL) {
        return NULL;
    }

    char authorization[WB_AUTH_CAP];
    int written = snprintf(authorization, sizeof(authorization), "Bearer %s",
                           CONFIG_WB_DEVICE_TOKEN);
    if (written < 0 || (size_t)written >= sizeof(authorization) ||
        esp_http_client_set_header(client, "Authorization", authorization) != ESP_OK ||
        esp_http_client_set_header(client, "Accept", "application/json") != ESP_OK) {
        esp_http_client_cleanup(client);
        return NULL;
    }
    char request_id[WB_ID_CAP];
    (void)snprintf(request_id, sizeof(request_id), "passport-%08lx-%08lx",
                   (unsigned long)esp_random(), (unsigned long)esp_random());
    if (esp_http_client_set_header(client, "X-Request-Id", request_id) != ESP_OK) {
        esp_http_client_cleanup(client);
        return NULL;
    }
    return client;
}

static esp_err_t write_all(esp_http_client_handle_t client,
                           const void *data, size_t bytes)
{
    const char *cursor = (const char *)data;
    size_t written = 0U;
    while (written < bytes) {
        int result = esp_http_client_write(client, cursor + written,
                                           (int)(bytes - written));
        if (result <= 0) {
            return ESP_FAIL;
        }
        written += (size_t)result;
    }
    return ESP_OK;
}

static esp_err_t read_bounded(esp_http_client_handle_t client,
                              char *output, size_t capacity, size_t *length)
{
    size_t used = 0U;
    if (capacity == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    for (;;) {
        if (used + 1U >= capacity) {
            char probe;
            int extra = esp_http_client_read(client, &probe, 1);
            output[used] = '\0';
            if (length != NULL) {
                *length = used;
            }
            return extra == 0 ? ESP_OK : ESP_ERR_INVALID_SIZE;
        }
        int received = esp_http_client_read(client, &output[used],
                                            (int)(capacity - used - 1U));
        if (received < 0) {
            output[used] = '\0';
            return ESP_FAIL;
        }
        if (received == 0) {
            break;
        }
        used += (size_t)received;
    }
    output[used] = '\0';
    if (length != NULL) {
        *length = used;
    }
    return ESP_OK;
}

static esp_err_t finish_response(esp_http_client_handle_t client,
                                 char *response, size_t response_capacity,
                                 size_t *response_length, int *status_code)
{
    int64_t headers = esp_http_client_fetch_headers(client);
    if (headers < 0) {
        return ESP_FAIL;
    }
    if (headers >= (int64_t)response_capacity) {
        return ESP_ERR_INVALID_SIZE;
    }
    esp_err_t error = read_bounded(client, response, response_capacity,
                                   response_length);
    if (status_code != NULL) {
        *status_code = esp_http_client_get_status_code(client);
    }
    return error;
}

static esp_err_t exchange_locked(const char *url,
                                 esp_http_client_method_t method,
                                 const char *content_type,
                                 const void *body, size_t body_length,
                                 char *response, size_t response_capacity,
                                 size_t *response_length, int *status_code)
{
    esp_http_client_handle_t client = make_client(url, method);
    if (client == NULL) {
        return ESP_ERR_NO_MEM;
    }
    if (content_type != NULL &&
        esp_http_client_set_header(client, "Content-Type", content_type) != ESP_OK) {
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }
    esp_err_t error = esp_http_client_open(client, (int)body_length);
    if (error == ESP_OK && body_length > 0U) {
        error = write_all(client, body, body_length);
    }
    if (error == ESP_OK) {
        error = finish_response(client, response, response_capacity,
                                response_length, status_code);
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return error;
}

static bool json_string(cJSON *root, const char *name, char *output,
                        size_t capacity, bool required)
{
    cJSON *value = cJSON_GetObjectItemCaseSensitive(root, name);
    if (value == NULL && !required) {
        output[0] = '\0';
        return true;
    }
    if (!cJSON_IsString(value) || value->valuestring == NULL ||
        strlen(value->valuestring) >= capacity || !utf8_valid(value->valuestring)) {
        return false;
    }
    memcpy(output, value->valuestring, strlen(value->valuestring) + 1U);
    return true;
}

static bool parse_result(const char *body, size_t length,
                         wb_transport_result_t *result)
{
    cJSON *root = cJSON_ParseWithLengthOpts(body, length, NULL, false);
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return false;
    }
    memset(result, 0, sizeof(*result));
    cJSON *status = cJSON_GetObjectItemCaseSensitive(root, "status");
    if (!cJSON_IsString(status) || status->valuestring == NULL) {
        cJSON_Delete(root);
        return false;
    }
    if (strcmp(status->valuestring, "PENDING") == 0) {
        result->status = WB_OPERATION_PENDING;
    } else if (strcmp(status->valuestring, "SUCCEEDED") == 0) {
        result->status = WB_OPERATION_SUCCEEDED;
    } else if (strcmp(status->valuestring, "FAILED") == 0) {
        result->status = WB_OPERATION_FAILED;
    } else {
        cJSON_Delete(root);
        return false;
    }
    bool valid = json_string(root, "receipt_id", result->receipt_id,
                             sizeof(result->receipt_id), false) &&
                 json_string(root, "error_code", result->error_code,
                             sizeof(result->error_code), false);
    cJSON *retryable = cJSON_GetObjectItemCaseSensitive(root, "retryable");
    if (retryable != NULL) {
        if (!cJSON_IsBool(retryable)) {
            valid = false;
        } else {
            result->retryable = cJSON_IsTrue(retryable);
        }
    }
    if ((result->status == WB_OPERATION_SUCCEEDED && result->receipt_id[0] == '\0') ||
        (result->status == WB_OPERATION_FAILED && result->error_code[0] == '\0')) {
        valid = false;
    }
    cJSON_Delete(root);
    return valid;
}

static void parse_error(const char *body, size_t length,
                        wb_transport_result_t *result)
{
    memset(result, 0, sizeof(*result));
    result->status = WB_OPERATION_FAILED;
    (void)snprintf(result->error_code, sizeof(result->error_code), "gateway_error");
    cJSON *root = cJSON_ParseWithLengthOpts(body, length, NULL, false);
    cJSON *error = cJSON_IsObject(root)
        ? cJSON_GetObjectItemCaseSensitive(root, "error") : NULL;
    if (cJSON_IsObject(error)) {
        (void)json_string(error, "code", result->error_code,
                          sizeof(result->error_code), false);
        cJSON *retryable = cJSON_GetObjectItemCaseSensitive(error, "retryable");
        result->retryable = cJSON_IsTrue(retryable);
    }
    if (result->retryable) {
        result->status = WB_OPERATION_PENDING;
    }
    cJSON_Delete(root);
}

esp_err_t wb_transport_init(void)
{
#if CONFIG_WB_DEMO_MODE
    return ESP_OK;
#else
    if (!transport_configuration_valid()) {
        ESP_LOGE(TAG, "gateway configuration is missing or unsafe");
        return ESP_ERR_INVALID_ARG;
    }
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
    }
    if (s_snapshot_lock == NULL) {
        s_snapshot_lock = xSemaphoreCreateMutex();
    }
    return s_lock != NULL && s_snapshot_lock != NULL ? ESP_OK : ESP_ERR_NO_MEM;
#endif
}

esp_err_t wb_transport_fetch_snapshot(const char *after_cursor,
                                      wb_snapshot_t *snapshot)
{
    if (snapshot == NULL || s_snapshot_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    char url[WB_URL_CAP];
    if (!make_url(url, sizeof(url), "/v1/snapshot", "after", after_cursor)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (xSemaphoreTake(s_snapshot_lock,
                       pdMS_TO_TICKS(CONFIG_WB_HTTP_TIMEOUT_MS)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    size_t response_length = 0U;
    int status = 0;
    esp_err_t error = exchange_locked(url, HTTP_METHOD_GET, NULL, NULL, 0U,
                                      s_snapshot_body, sizeof(s_snapshot_body),
                                      &response_length, &status);
    if (error != ESP_OK) {
        xSemaphoreGive(s_snapshot_lock);
        return error;
    }
    if (status != 200) {
        ESP_LOGW(TAG, "snapshot failed: HTTP %d (%u bytes)", status,
                 (unsigned)response_length);
        xSemaphoreGive(s_snapshot_lock);
        return ESP_FAIL;
    }
    wb_protocol_result_t parsed = wb_protocol_parse_snapshot(
        s_snapshot_body, response_length, snapshot);
    xSemaphoreGive(s_snapshot_lock);
    if (parsed != WB_PROTOCOL_OK) {
        ESP_LOGW(TAG, "snapshot schema rejected: %s", wb_protocol_result_name(parsed));
        return ESP_ERR_INVALID_RESPONSE;
    }
    return ESP_OK;
}

esp_err_t wb_transport_submit_action(const wb_action_t *action,
                                     wb_transport_result_t *result)
{
    if (action == NULL || result == NULL || s_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    size_t request_length = 0U;
    if (wb_protocol_serialize_action(action, s_action_body, sizeof(s_action_body),
                                     &request_length) != WB_PROTOCOL_OK) {
        return ESP_ERR_INVALID_ARG;
    }
    char url[WB_URL_CAP];
    if (!make_url(url, sizeof(url), "/v1/actions", NULL, NULL)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(CONFIG_WB_HTTP_TIMEOUT_MS)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    size_t response_length = 0U;
    int status = 0;
    esp_err_t error = exchange_locked(url, HTTP_METHOD_POST, "application/json",
                                      s_action_body, request_length,
                                      s_small_body, sizeof(s_small_body),
                                      &response_length, &status);
    if (error != ESP_OK) {
        xSemaphoreGive(s_lock);
        return error;
    }
    if (status < 200 || status >= 300) {
        parse_error(s_small_body, response_length, result);
        xSemaphoreGive(s_lock);
        ESP_LOGW(TAG, "action failed: HTTP %d code=%s", status, result->error_code);
        return ESP_FAIL;
    }
    bool valid = parse_result(s_small_body, response_length, result);
    xSemaphoreGive(s_lock);
    return valid ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
}

esp_err_t wb_transport_get_operation(const char *operation_id,
                                     wb_transport_result_t *result)
{
    if (operation_id == NULL || operation_id[0] == '\0' || result == NULL ||
        s_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    char url[WB_URL_CAP];
    if (!make_operation_url(url, sizeof(url), operation_id)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(CONFIG_WB_HTTP_TIMEOUT_MS)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    size_t response_length = 0U;
    int status = 0;
    esp_err_t error = exchange_locked(url, HTTP_METHOD_GET, NULL, NULL, 0U,
                                      s_small_body, sizeof(s_small_body),
                                      &response_length, &status);
    if (error != ESP_OK) {
        xSemaphoreGive(s_lock);
        return error;
    }
    if (status != 200) {
        parse_error(s_small_body, response_length, result);
        xSemaphoreGive(s_lock);
        return ESP_FAIL;
    }
    bool valid = parse_result(s_small_body, response_length, result);
    xSemaphoreGive(s_lock);
    return valid ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
}

esp_err_t wb_transport_transcription_begin(const char *operation_id,
                                           size_t total_pcm_bytes)
{
    if (operation_id == NULL || operation_id[0] == '\0' || s_lock == NULL ||
        total_pcm_bytes < WB_PCM_BYTES_PER_SECOND ||
        total_pcm_bytes > WB_PCM_MAX_BYTES || (total_pcm_bytes & 1U) != 0U ||
        s_upload != NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(CONFIG_WB_HTTP_TIMEOUT_MS)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    s_upload_owns_lock = true;

    char url[WB_URL_CAP];
    if (!make_url(url, sizeof(url), "/v1/transcriptions", "operation_id",
                  operation_id)) {
        wb_transport_transcription_abort();
        return ESP_ERR_INVALID_ARG;
    }
    s_upload = make_client(url, HTTP_METHOD_POST);
    if (s_upload == NULL ||
        esp_http_client_set_header(s_upload, "Content-Type",
                                   "audio/L16;rate=16000;channels=1") != ESP_OK) {
        wb_transport_transcription_abort();
        return ESP_ERR_NO_MEM;
    }
    esp_err_t error = esp_http_client_open(s_upload, (int)total_pcm_bytes);
    if (error != ESP_OK) {
        wb_transport_transcription_abort();
        return error;
    }
    s_upload_expected = total_pcm_bytes;
    s_upload_written = 0U;
    return ESP_OK;
}

esp_err_t wb_transport_transcription_write(const void *pcm, size_t bytes)
{
    if (s_upload == NULL || pcm == NULL || bytes == 0U ||
        bytes > s_upload_expected - s_upload_written) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t error = write_all(s_upload, pcm, bytes);
    if (error == ESP_OK) {
        s_upload_written += bytes;
    }
    return error;
}

esp_err_t wb_transport_transcription_finish(char *transcript,
                                            size_t transcript_capacity)
{
    if (s_upload == NULL || transcript == NULL || transcript_capacity == 0U ||
        s_upload_written != s_upload_expected) {
        return ESP_ERR_INVALID_STATE;
    }
    size_t response_length = 0U;
    int status = 0;
    esp_err_t error = finish_response(s_upload, s_small_body, sizeof(s_small_body),
                                      &response_length, &status);
    esp_http_client_close(s_upload);
    esp_http_client_cleanup(s_upload);
    s_upload = NULL;
    s_upload_expected = 0U;
    s_upload_written = 0U;
    if (error != ESP_OK) {
        if (s_upload_owns_lock) {
            s_upload_owns_lock = false;
            xSemaphoreGive(s_lock);
        }
        return error;
    }
    if (status != 200) {
        ESP_LOGW(TAG, "transcription failed: HTTP %d (%u bytes)", status,
                 (unsigned)response_length);
        if (s_upload_owns_lock) {
            s_upload_owns_lock = false;
            xSemaphoreGive(s_lock);
        }
        return ESP_FAIL;
    }

    cJSON *root = cJSON_ParseWithLengthOpts(s_small_body, response_length, NULL, false);
    bool valid = cJSON_IsObject(root) &&
        json_string(root, "text", transcript, transcript_capacity, true) &&
        transcript[0] != '\0';
    cJSON_Delete(root);
    if (s_upload_owns_lock) {
        s_upload_owns_lock = false;
        xSemaphoreGive(s_lock);
    }
    return valid ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
}

void wb_transport_transcription_abort(void)
{
    if (s_upload != NULL) {
        esp_http_client_close(s_upload);
        esp_http_client_cleanup(s_upload);
        s_upload = NULL;
    }
    s_upload_expected = 0U;
    s_upload_written = 0U;
    if (s_upload_owns_lock && s_lock != NULL) {
        s_upload_owns_lock = false;
        xSemaphoreGive(s_lock);
    }
}
