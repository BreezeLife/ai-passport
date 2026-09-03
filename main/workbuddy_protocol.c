#include "workbuddy_protocol.h"

#include <ctype.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "workbuddy_model.h"

typedef struct {
    char *output;
    size_t capacity;
    size_t length;
    bool overflow;
} json_writer_t;

static wb_protocol_allocate_fn s_allocate = malloc;
static wb_protocol_deallocate_fn s_deallocate = free;
static bool s_allocation_failed;

static void *tracked_allocate(size_t size)
{
    void *allocation = s_allocate(size);

    if (allocation == NULL) {
        s_allocation_failed = true;
    }
    return allocation;
}

static void tracked_deallocate(void *pointer)
{
    s_deallocate(pointer);
}

static void install_cjson_hooks(void)
{
    cJSON_Hooks hooks = {
        .malloc_fn = tracked_allocate,
        .free_fn = tracked_deallocate,
    };

    cJSON_InitHooks(&hooks);
}

void wb_protocol_set_allocator(wb_protocol_allocate_fn allocate,
                               wb_protocol_deallocate_fn deallocate)
{
    if (allocate == NULL || deallocate == NULL) {
        s_allocate = malloc;
        s_deallocate = free;
    } else {
        s_allocate = allocate;
        s_deallocate = deallocate;
    }
    install_cjson_hooks();
}

void wb_protocol_reset_allocator(void)
{
    wb_protocol_set_allocator(malloc, free);
}

static size_t strict_utf8_codepoint_bytes(const unsigned char *text, size_t remaining)
{
    if (text[0] < 0x80U) {
        return 1U;
    }
    if (remaining >= 2U && text[0] >= 0xC2U && text[0] <= 0xDFU &&
        text[1] >= 0x80U && text[1] <= 0xBFU) {
        return 2U;
    }
    if (remaining >= 3U && text[0] == 0xE0U &&
        text[1] >= 0xA0U && text[1] <= 0xBFU &&
        text[2] >= 0x80U && text[2] <= 0xBFU) {
        return 3U;
    }
    if (remaining >= 3U &&
        ((text[0] >= 0xE1U && text[0] <= 0xECU) ||
         (text[0] >= 0xEEU && text[0] <= 0xEFU)) &&
        text[1] >= 0x80U && text[1] <= 0xBFU &&
        text[2] >= 0x80U && text[2] <= 0xBFU) {
        return 3U;
    }
    if (remaining >= 3U && text[0] == 0xEDU &&
        text[1] >= 0x80U && text[1] <= 0x9FU &&
        text[2] >= 0x80U && text[2] <= 0xBFU) {
        return 3U;
    }
    if (remaining >= 4U && text[0] == 0xF0U &&
        text[1] >= 0x90U && text[1] <= 0xBFU &&
        text[2] >= 0x80U && text[2] <= 0xBFU &&
        text[3] >= 0x80U && text[3] <= 0xBFU) {
        return 4U;
    }
    if (remaining >= 4U && text[0] >= 0xF1U && text[0] <= 0xF3U &&
        text[1] >= 0x80U && text[1] <= 0xBFU &&
        text[2] >= 0x80U && text[2] <= 0xBFU &&
        text[3] >= 0x80U && text[3] <= 0xBFU) {
        return 4U;
    }
    if (remaining >= 4U && text[0] == 0xF4U &&
        text[1] >= 0x80U && text[1] <= 0x8FU &&
        text[2] >= 0x80U && text[2] <= 0xBFU &&
        text[3] >= 0x80U && text[3] <= 0xBFU) {
        return 4U;
    }
    return 0U;
}

static bool strict_utf8_is_valid(const char *text, size_t length)
{
    size_t offset = 0U;

    while (offset < length) {
        size_t codepoint_bytes = strict_utf8_codepoint_bytes(
            (const unsigned char *)&text[offset], length - offset);

        if (codepoint_bytes == 0U) {
            return false;
        }
        offset += codepoint_bytes;
    }
    return true;
}

static wb_protocol_result_t validate_json_envelope(const char *json, size_t length)
{
    char containers[WB_PROTOCOL_MAX_JSON_DEPTH];
    size_t depth = 0U;
    size_t index = 0U;
    bool in_string = false;
    bool escaped = false;

    while (index < length) {
        unsigned char byte = (unsigned char)json[index];

        if (byte >= 0x80U) {
            size_t codepoint_bytes = strict_utf8_codepoint_bytes(
                (const unsigned char *)&json[index], length - index);

            if (codepoint_bytes == 0U) {
                return WB_PROTOCOL_ERR_INVALID_UTF8;
            }
            index += codepoint_bytes;
            continue;
        }

        if (in_string) {
            if (escaped) {
                escaped = false;
            } else if (byte == '\\') {
                escaped = true;
            } else if (byte == '"') {
                in_string = false;
            } else if (byte < 0x20U) {
                return WB_PROTOCOL_ERR_INVALID_JSON;
            }
        } else if (byte == '"') {
            in_string = true;
        } else if (byte == '{' || byte == '[') {
            if (depth >= WB_PROTOCOL_MAX_JSON_DEPTH) {
                return WB_PROTOCOL_ERR_LIMIT;
            }
            containers[depth++] = (char)byte;
        } else if (byte == '}' || byte == ']') {
            char expected = byte == '}' ? '{' : '[';

            if (depth == 0U || containers[depth - 1U] != expected) {
                return WB_PROTOCOL_ERR_INVALID_JSON;
            }
            --depth;
        } else if (byte < 0x20U && isspace(byte) == 0) {
            return WB_PROTOCOL_ERR_INVALID_JSON;
        }
        ++index;
    }

    if (in_string || escaped || depth != 0U) {
        return WB_PROTOCOL_ERR_INVALID_JSON;
    }
    return WB_PROTOCOL_OK;
}

static cJSON *object_item(const cJSON *object,
                          const char *first,
                          const char *second,
                          const char *third)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(object, first);

    if (item == NULL && second != NULL) {
        item = cJSON_GetObjectItemCaseSensitive(object, second);
    }
    if (item == NULL && third != NULL) {
        item = cJSON_GetObjectItemCaseSensitive(object, third);
    }
    return item;
}

static wb_protocol_result_t copy_string_item(const cJSON *item,
                                             char *destination,
                                             size_t capacity,
                                             bool strict_limit,
                                             bool require_nonempty)
{
    size_t length;
    size_t read_index = 0U;
    size_t write_index = 0U;

    if (!cJSON_IsString(item) || item->valuestring == NULL) {
        return WB_PROTOCOL_ERR_TYPE;
    }
    length = strlen(item->valuestring);
    if (!strict_utf8_is_valid(item->valuestring, length)) {
        return WB_PROTOCOL_ERR_INVALID_UTF8;
    }
    if (require_nonempty && length == 0U) {
        return WB_PROTOCOL_ERR_MISSING_FIELD;
    }
    if (strict_limit && length >= capacity) {
        return WB_PROTOCOL_ERR_LIMIT;
    }

    while (read_index < length) {
        size_t codepoint_bytes = strict_utf8_codepoint_bytes(
            (const unsigned char *)&item->valuestring[read_index], length - read_index);

        if (write_index + codepoint_bytes >= capacity) {
            break;
        }
        memcpy(&destination[write_index], &item->valuestring[read_index], codepoint_bytes);
        read_index += codepoint_bytes;
        write_index += codepoint_bytes;
    }
    destination[write_index] = '\0';
    return WB_PROTOCOL_OK;
}

static wb_protocol_result_t read_required_string(const cJSON *object,
                                                 const char *first,
                                                 const char *second,
                                                 const char *third,
                                                 char *destination,
                                                 size_t capacity,
                                                 bool strict_limit,
                                                 bool require_nonempty)
{
    cJSON *item = object_item(object, first, second, third);

    if (item == NULL) {
        return WB_PROTOCOL_ERR_MISSING_FIELD;
    }
    return copy_string_item(item, destination, capacity, strict_limit, require_nonempty);
}

static wb_protocol_result_t read_optional_string(const cJSON *object,
                                                 const char *first,
                                                 const char *second,
                                                 char *destination,
                                                 size_t capacity,
                                                 bool strict_limit)
{
    cJSON *item = object_item(object, first, second, NULL);

    if (item == NULL) {
        destination[0] = '\0';
        return WB_PROTOCOL_OK;
    }
    return copy_string_item(item, destination, capacity, strict_limit, false);
}

static wb_protocol_result_t read_required_bool(const cJSON *object,
                                               const char *name,
                                               bool *value)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);

    if (item == NULL) {
        return WB_PROTOCOL_ERR_MISSING_FIELD;
    }
    if (!cJSON_IsBool(item)) {
        return WB_PROTOCOL_ERR_TYPE;
    }
    *value = cJSON_IsTrue(item);
    return WB_PROTOCOL_OK;
}

static wb_protocol_result_t number_to_unsigned(const cJSON *item, unsigned *value)
{
    double number;

    if (!cJSON_IsNumber(item)) {
        return WB_PROTOCOL_ERR_TYPE;
    }
    number = item->valuedouble;
    if (number < 0.0) {
        return WB_PROTOCOL_ERR_TYPE;
    }
    if (number > (double)UINT_MAX) {
        return WB_PROTOCOL_ERR_LIMIT;
    }
    *value = (unsigned)number;
    if ((double)*value != number) {
        return WB_PROTOCOL_ERR_TYPE;
    }
    return WB_PROTOCOL_OK;
}

static wb_protocol_result_t read_required_unsigned(const cJSON *object,
                                                   const char *name,
                                                   unsigned *value)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);

    if (item == NULL) {
        return WB_PROTOCOL_ERR_MISSING_FIELD;
    }
    return number_to_unsigned(item, value);
}

static wb_protocol_result_t read_optional_unsigned(const cJSON *object,
                                                   const char *name,
                                                   unsigned *value)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);

    if (item == NULL) {
        return WB_PROTOCOL_OK;
    }
    return number_to_unsigned(item, value);
}

static bool ascii_equal(const char *left, const char *right)
{
    while (*left != '\0' && *right != '\0') {
        if (toupper((unsigned char)*left) != toupper((unsigned char)*right)) {
            return false;
        }
        ++left;
        ++right;
    }
    return *left == '\0' && *right == '\0';
}

static wb_message_role_t normalize_role(const char *role)
{
    if (ascii_equal(role, "assistant")) {
        return WB_MESSAGE_ROLE_ASSISTANT;
    }
    if (ascii_equal(role, "user")) {
        return WB_MESSAGE_ROLE_USER;
    }
    if (ascii_equal(role, "system")) {
        return WB_MESSAGE_ROLE_SYSTEM;
    }
    return WB_MESSAGE_ROLE_UNKNOWN;
}

static wb_output_kind_t normalize_output_kind(const char *kind)
{
    if (ascii_equal(kind, "plan")) {
        return WB_OUTPUT_KIND_PLAN;
    }
    if (ascii_equal(kind, "checklist")) {
        return WB_OUTPUT_KIND_CHECKLIST;
    }
    if (ascii_equal(kind, "overview")) {
        return WB_OUTPUT_KIND_OVERVIEW;
    }
    if (ascii_equal(kind, "image")) {
        return WB_OUTPUT_KIND_IMAGE;
    }
    if (ascii_equal(kind, "document")) {
        return WB_OUTPUT_KIND_DOCUMENT;
    }
    return WB_OUTPUT_KIND_UNKNOWN;
}

static wb_protocol_result_t parse_message(const cJSON *item, wb_message_t *message)
{
    wb_protocol_result_t result;
    char role[32];
    cJSON *unread;

    if (!cJSON_IsObject(item)) {
        return WB_PROTOCOL_ERR_TYPE;
    }
    result = read_required_string(item, "id", "message_id", NULL,
                                  message->id, sizeof(message->id), true, true);
    if (result != WB_PROTOCOL_OK) {
        return result;
    }
    result = read_optional_string(item, "title", "sender",
                                  message->title, sizeof(message->title), false);
    if (result != WB_PROTOCOL_OK) {
        return result;
    }
    result = read_optional_string(item, "preview", NULL,
                                  message->preview, sizeof(message->preview), false);
    if (result != WB_PROTOCOL_OK) {
        return result;
    }
    result = read_optional_string(item, "role", "sender_role",
                                  role, sizeof(role), true);
    if (result != WB_PROTOCOL_OK) {
        return result;
    }
    message->role = normalize_role(role);
    unread = cJSON_GetObjectItemCaseSensitive(item, "unread");
    if (unread != NULL) {
        if (!cJSON_IsBool(unread)) {
            return WB_PROTOCOL_ERR_TYPE;
        }
        message->unread = cJSON_IsTrue(unread);
    }
    return WB_PROTOCOL_OK;
}

static wb_protocol_result_t parse_task(const cJSON *item, wb_task_t *task)
{
    wb_protocol_result_t result;
    char status[32];

    if (!cJSON_IsObject(item)) {
        return WB_PROTOCOL_ERR_TYPE;
    }
    result = read_required_string(item, "id", "task_id", NULL,
                                  task->id, sizeof(task->id), true, true);
    if (result != WB_PROTOCOL_OK) {
        return result;
    }
    result = read_optional_string(item, "title", NULL,
                                  task->title, sizeof(task->title), false);
    if (result != WB_PROTOCOL_OK) {
        return result;
    }
    result = read_optional_string(item, "preview", "description",
                                  task->preview, sizeof(task->preview), false);
    if (result != WB_PROTOCOL_OK) {
        return result;
    }
    result = read_required_string(item, "status", NULL, NULL,
                                  status, sizeof(status), true, true);
    if (result != WB_PROTOCOL_OK) {
        return result;
    }
    task->status = wb_task_status_normalize(status);
    return WB_PROTOCOL_OK;
}

static wb_protocol_result_t parse_output(const cJSON *item, wb_output_t *output)
{
    wb_protocol_result_t result;
    char kind[32];

    if (!cJSON_IsObject(item)) {
        return WB_PROTOCOL_ERR_TYPE;
    }
    result = read_required_string(item, "id", "output_id", "artifact_id",
                                  output->id, sizeof(output->id), true, true);
    if (result != WB_PROTOCOL_OK) {
        return result;
    }
    result = read_optional_string(item, "task_id", NULL,
                                  output->task_id, sizeof(output->task_id), true);
    if (result != WB_PROTOCOL_OK) {
        return result;
    }
    result = read_optional_string(item, "title", NULL,
                                  output->title, sizeof(output->title), false);
    if (result != WB_PROTOCOL_OK) {
        return result;
    }
    result = read_optional_string(item, "preview", "description",
                                  output->preview, sizeof(output->preview), false);
    if (result != WB_PROTOCOL_OK) {
        return result;
    }
    result = read_optional_string(item, "kind", "type",
                                  kind, sizeof(kind), true);
    if (result != WB_PROTOCOL_OK) {
        return result;
    }
    output->kind = normalize_output_kind(kind);
    return WB_PROTOCOL_OK;
}

static wb_protocol_result_t parse_messages(const cJSON *array, wb_snapshot_t *snapshot)
{
    int count;
    int index;

    if (!cJSON_IsArray(array)) {
        return WB_PROTOCOL_ERR_TYPE;
    }
    count = cJSON_GetArraySize(array);
    snapshot->message_count = count < (int)WB_MAX_ITEMS ? (size_t)count : WB_MAX_ITEMS;
    for (index = 0; index < (int)snapshot->message_count; ++index) {
        wb_protocol_result_t result = parse_message(cJSON_GetArrayItem(array, index),
                                                    &snapshot->messages[index]);

        if (result != WB_PROTOCOL_OK) {
            return result;
        }
    }
    return WB_PROTOCOL_OK;
}

static wb_protocol_result_t parse_tasks(const cJSON *array, wb_snapshot_t *snapshot)
{
    int count;
    int index;

    if (!cJSON_IsArray(array)) {
        return WB_PROTOCOL_ERR_TYPE;
    }
    count = cJSON_GetArraySize(array);
    snapshot->task_count = count < (int)WB_MAX_ITEMS ? (size_t)count : WB_MAX_ITEMS;
    for (index = 0; index < (int)snapshot->task_count; ++index) {
        wb_protocol_result_t result = parse_task(cJSON_GetArrayItem(array, index),
                                                 &snapshot->tasks[index]);

        if (result != WB_PROTOCOL_OK) {
            return result;
        }
    }
    return WB_PROTOCOL_OK;
}

static wb_protocol_result_t parse_outputs(const cJSON *array, wb_snapshot_t *snapshot)
{
    int count;
    int index;

    if (!cJSON_IsArray(array)) {
        return WB_PROTOCOL_ERR_TYPE;
    }
    count = cJSON_GetArraySize(array);
    snapshot->output_count = count < (int)WB_MAX_ITEMS ? (size_t)count : WB_MAX_ITEMS;
    for (index = 0; index < (int)snapshot->output_count; ++index) {
        wb_protocol_result_t result = parse_output(cJSON_GetArrayItem(array, index),
                                                   &snapshot->outputs[index]);

        if (result != WB_PROTOCOL_OK) {
            return result;
        }
    }
    return WB_PROTOCOL_OK;
}

static wb_protocol_result_t snapshot_from_dom(const cJSON *root, wb_snapshot_t *snapshot)
{
    wb_protocol_result_t result;
    cJSON *fresh;
    cJSON *stale;
    cJSON *messages;
    cJSON *tasks;
    cJSON *outputs;

    if (!cJSON_IsObject(root)) {
        return WB_PROTOCOL_ERR_TYPE;
    }
    result = read_required_unsigned(root, "version", &snapshot->version);
    if (result != WB_PROTOCOL_OK) {
        return result;
    }
    if (snapshot->version != 1U) {
        return WB_PROTOCOL_ERR_UNSUPPORTED_VERSION;
    }
    result = read_required_string(root, "request_id", NULL, NULL,
                                  snapshot->request_id, sizeof(snapshot->request_id),
                                  true, false);
    if (result != WB_PROTOCOL_OK) {
        return result;
    }
    result = read_required_string(root, "cursor", NULL, NULL,
                                  snapshot->cursor, sizeof(snapshot->cursor), true, false);
    if (result != WB_PROTOCOL_OK) {
        return result;
    }

    fresh = cJSON_GetObjectItemCaseSensitive(root, "fresh");
    stale = cJSON_GetObjectItemCaseSensitive(root, "stale");
    if (fresh != NULL) {
        if (!cJSON_IsBool(fresh)) {
            return WB_PROTOCOL_ERR_TYPE;
        }
        snapshot->fresh = cJSON_IsTrue(fresh);
    } else if (stale != NULL) {
        if (!cJSON_IsBool(stale)) {
            return WB_PROTOCOL_ERR_TYPE;
        }
        snapshot->fresh = !cJSON_IsTrue(stale);
    } else {
        return WB_PROTOCOL_ERR_MISSING_FIELD;
    }

    result = read_required_bool(root, "assistant_available",
                                &snapshot->assistant_available);
    if (result != WB_PROTOCOL_OK) {
        return result;
    }
    result = read_optional_unsigned(root, "unread_count", &snapshot->unread_count);
    if (result != WB_PROTOCOL_OK) {
        return result;
    }
    result = read_optional_unsigned(root, "active_task_count",
                                    &snapshot->active_task_count);
    if (result != WB_PROTOCOL_OK) {
        return result;
    }

    messages = cJSON_GetObjectItemCaseSensitive(root, "messages");
    tasks = cJSON_GetObjectItemCaseSensitive(root, "tasks");
    outputs = object_item(root, "outputs", "artifacts", NULL);
    if (messages == NULL || tasks == NULL || outputs == NULL) {
        return WB_PROTOCOL_ERR_MISSING_FIELD;
    }
    result = parse_messages(messages, snapshot);
    if (result != WB_PROTOCOL_OK) {
        return result;
    }
    result = parse_tasks(tasks, snapshot);
    if (result != WB_PROTOCOL_OK) {
        return result;
    }
    return parse_outputs(outputs, snapshot);
}

wb_protocol_result_t wb_protocol_parse_snapshot(const char *json,
                                                size_t json_length,
                                                wb_snapshot_t *snapshot)
{
    const char *parse_end = NULL;
    cJSON *root;
    wb_snapshot_t *parsed;
    wb_protocol_result_t result;

    if (json == NULL || snapshot == NULL || json_length == 0U) {
        return WB_PROTOCOL_ERR_ARGUMENT;
    }
    if (json_length > WB_SNAPSHOT_MAX_BYTES) {
        return WB_PROTOCOL_ERR_LIMIT;
    }
    result = validate_json_envelope(json, json_length);
    if (result != WB_PROTOCOL_OK) {
        return result;
    }

    install_cjson_hooks();
    s_allocation_failed = false;
    root = cJSON_ParseWithLengthOpts(json, json_length, &parse_end, false);
    if (root == NULL) {
        return s_allocation_failed ? WB_PROTOCOL_ERR_NO_MEMORY
                                   : WB_PROTOCOL_ERR_INVALID_JSON;
    }
    while (parse_end != NULL && parse_end < json + json_length &&
           isspace((unsigned char)*parse_end) != 0) {
        ++parse_end;
    }
    if (parse_end == NULL || parse_end != json + json_length) {
        cJSON_Delete(root);
        return WB_PROTOCOL_ERR_INVALID_JSON;
    }

    parsed = tracked_allocate(sizeof(*parsed));
    if (parsed == NULL) {
        cJSON_Delete(root);
        return WB_PROTOCOL_ERR_NO_MEMORY;
    }
    memset(parsed, 0, sizeof(*parsed));

    result = snapshot_from_dom(root, parsed);
    cJSON_Delete(root);
    if (result == WB_PROTOCOL_OK) {
        *snapshot = *parsed;
    }
    tracked_deallocate(parsed);
    return result;
}

static bool bounded_string_length(const char *text, size_t maximum, size_t *length)
{
    size_t index;

    if (text == NULL) {
        return false;
    }
    for (index = 0U; index <= maximum; ++index) {
        if (text[index] == '\0') {
            if (length != NULL) {
                *length = index;
            }
            return true;
        }
    }
    return false;
}

static void writer_byte(json_writer_t *writer, char byte)
{
    if (writer->length + 1U < writer->capacity) {
        writer->output[writer->length] = byte;
    } else {
        writer->overflow = true;
    }
    ++writer->length;
}

static void writer_text(json_writer_t *writer, const char *text)
{
    while (*text != '\0') {
        writer_byte(writer, *text++);
    }
}

static void writer_hex_control(json_writer_t *writer, unsigned char byte)
{
    static const char hex[] = "0123456789abcdef";

    writer_text(writer, "\\u00");
    writer_byte(writer, hex[byte >> 4]);
    writer_byte(writer, hex[byte & 0x0FU]);
}

static void writer_json_string(json_writer_t *writer, const char *text)
{
    writer_byte(writer, '"');
    while (*text != '\0') {
        unsigned char byte = (unsigned char)*text++;

        switch (byte) {
        case '"':
            writer_text(writer, "\\\"");
            break;
        case '\\':
            writer_text(writer, "\\\\");
            break;
        case '\b':
            writer_text(writer, "\\b");
            break;
        case '\f':
            writer_text(writer, "\\f");
            break;
        case '\n':
            writer_text(writer, "\\n");
            break;
        case '\r':
            writer_text(writer, "\\r");
            break;
        case '\t':
            writer_text(writer, "\\t");
            break;
        default:
            if (byte < 0x20U) {
                writer_hex_control(writer, byte);
            } else {
                writer_byte(writer, (char)byte);
            }
            break;
        }
    }
    writer_byte(writer, '"');
}

static wb_protocol_result_t validate_action(const wb_action_t *action)
{
    size_t operation_length;
    size_t target_length = 0U;
    size_t text_length;

    if (action == NULL ||
        !bounded_string_length(action->operation_id, WB_ID_MAX_BYTES, &operation_length) ||
        !bounded_string_length(action->text, WB_TRANSCRIPT_MAX_BYTES, &text_length)) {
        return action == NULL ? WB_PROTOCOL_ERR_ARGUMENT : WB_PROTOCOL_ERR_LIMIT;
    }
    if (operation_length == 0U || text_length == 0U) {
        return WB_PROTOCOL_ERR_ARGUMENT;
    }
    if (!strict_utf8_is_valid(action->operation_id, operation_length) ||
        !strict_utf8_is_valid(action->text, text_length)) {
        return WB_PROTOCOL_ERR_INVALID_UTF8;
    }
    if (action->type == WB_ACTION_REPLY || action->type == WB_ACTION_TASK_FOLLOWUP) {
        if (!bounded_string_length(action->target_id, WB_ID_MAX_BYTES, &target_length)) {
            return WB_PROTOCOL_ERR_LIMIT;
        }
        if (target_length == 0U) {
            return WB_PROTOCOL_ERR_ARGUMENT;
        }
        if (!strict_utf8_is_valid(action->target_id, target_length)) {
            return WB_PROTOCOL_ERR_INVALID_UTF8;
        }
    } else if (action->type != WB_ACTION_TASK_CREATE) {
        return WB_PROTOCOL_ERR_ARGUMENT;
    }
    return WB_PROTOCOL_OK;
}

wb_protocol_result_t wb_protocol_serialize_action(const wb_action_t *action,
                                                  char *output,
                                                  size_t output_capacity,
                                                  size_t *output_length)
{
    json_writer_t writer;
    wb_protocol_result_t validation = validate_action(action);

    if (validation != WB_PROTOCOL_OK) {
        return validation;
    }
    if (output == NULL || output_capacity == 0U) {
        return WB_PROTOCOL_ERR_ARGUMENT;
    }

    writer.output = output;
    writer.capacity = output_capacity;
    writer.length = 0U;
    writer.overflow = false;
    writer_text(&writer, "{\"version\":1,\"operation_id\":");
    writer_json_string(&writer, action->operation_id);

    switch (action->type) {
    case WB_ACTION_REPLY:
        writer_text(&writer, ",\"type\":\"reply\",\"message_id\":");
        writer_json_string(&writer, action->target_id);
        writer_text(&writer, ",\"text\":");
        writer_json_string(&writer, action->text);
        break;
    case WB_ACTION_TASK_CREATE:
        writer_text(&writer, ",\"type\":\"task_create\",\"prompt\":");
        writer_json_string(&writer, action->text);
        break;
    case WB_ACTION_TASK_FOLLOWUP:
        writer_text(&writer, ",\"type\":\"task_followup\",\"task_id\":");
        writer_json_string(&writer, action->target_id);
        writer_text(&writer, ",\"text\":");
        writer_json_string(&writer, action->text);
        break;
    default:
        return WB_PROTOCOL_ERR_ARGUMENT;
    }
    writer_byte(&writer, '}');

    if (writer.length < writer.capacity) {
        writer.output[writer.length] = '\0';
    } else {
        writer.output[writer.capacity - 1U] = '\0';
        writer.overflow = true;
    }
    if (output_length != NULL) {
        *output_length = writer.length;
    }
    return writer.overflow ? WB_PROTOCOL_ERR_BUFFER_TOO_SMALL : WB_PROTOCOL_OK;
}

static wb_protocol_result_t serialize_parts(wb_action_type_t type,
                                            const char *operation_id,
                                            const char *target_id,
                                            const char *text,
                                            char *output,
                                            size_t output_capacity,
                                            size_t *output_length)
{
    wb_action_t action = { 0 };

    if (operation_id == NULL || text == NULL ||
        ((type == WB_ACTION_REPLY || type == WB_ACTION_TASK_FOLLOWUP) &&
         target_id == NULL)) {
        return WB_PROTOCOL_ERR_ARGUMENT;
    }
    if (!bounded_string_length(operation_id, WB_ID_MAX_BYTES, NULL) ||
        !bounded_string_length(text, WB_TRANSCRIPT_MAX_BYTES, NULL) ||
        ((type == WB_ACTION_REPLY || type == WB_ACTION_TASK_FOLLOWUP) &&
         !bounded_string_length(target_id, WB_ID_MAX_BYTES, NULL))) {
        return WB_PROTOCOL_ERR_LIMIT;
    }
    action.type = type;
    (void)snprintf(action.operation_id, sizeof(action.operation_id), "%s", operation_id);
    if (target_id != NULL) {
        (void)snprintf(action.target_id, sizeof(action.target_id), "%s", target_id);
    }
    (void)snprintf(action.text, sizeof(action.text), "%s", text);
    return wb_protocol_serialize_action(&action, output, output_capacity, output_length);
}

wb_protocol_result_t wb_protocol_serialize_reply(const char *operation_id,
                                                 const char *message_id,
                                                 const char *text,
                                                 char *output,
                                                 size_t output_capacity,
                                                 size_t *output_length)
{
    return serialize_parts(WB_ACTION_REPLY, operation_id, message_id, text,
                           output, output_capacity, output_length);
}

wb_protocol_result_t wb_protocol_serialize_task_create(const char *operation_id,
                                                       const char *prompt,
                                                       char *output,
                                                       size_t output_capacity,
                                                       size_t *output_length)
{
    return serialize_parts(WB_ACTION_TASK_CREATE, operation_id, NULL, prompt,
                           output, output_capacity, output_length);
}

wb_protocol_result_t wb_protocol_serialize_task_followup(const char *operation_id,
                                                         const char *task_id,
                                                         const char *text,
                                                         char *output,
                                                         size_t output_capacity,
                                                         size_t *output_length)
{
    return serialize_parts(WB_ACTION_TASK_FOLLOWUP, operation_id, task_id, text,
                           output, output_capacity, output_length);
}

const char *wb_protocol_result_name(wb_protocol_result_t result)
{
    switch (result) {
    case WB_PROTOCOL_OK:
        return "OK";
    case WB_PROTOCOL_ERR_ARGUMENT:
        return "ARGUMENT";
    case WB_PROTOCOL_ERR_INVALID_JSON:
        return "INVALID_JSON";
    case WB_PROTOCOL_ERR_INVALID_UTF8:
        return "INVALID_UTF8";
    case WB_PROTOCOL_ERR_UNSUPPORTED_VERSION:
        return "UNSUPPORTED_VERSION";
    case WB_PROTOCOL_ERR_MISSING_FIELD:
        return "MISSING_FIELD";
    case WB_PROTOCOL_ERR_TYPE:
        return "TYPE";
    case WB_PROTOCOL_ERR_LIMIT:
        return "LIMIT";
    case WB_PROTOCOL_ERR_BUFFER_TOO_SMALL:
        return "BUFFER_TOO_SMALL";
    case WB_PROTOCOL_ERR_NO_MEMORY:
        return "NO_MEMORY";
    default:
        return "UNKNOWN";
    }
}
