#include "workbuddy_protocol.h"

#include <ctype.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "workbuddy_model.h"

typedef struct {
    const char *current;
    const char *end;
    wb_protocol_result_t error;
} json_reader_t;

typedef struct {
    char *output;
    size_t capacity;
    size_t length;
    bool overflow;
} json_writer_t;

enum {
    SNAPSHOT_HAS_VERSION = 1U << 0,
    SNAPSHOT_HAS_REQUEST_ID = 1U << 1,
    SNAPSHOT_HAS_CURSOR = 1U << 2,
    SNAPSHOT_HAS_FRESHNESS = 1U << 3,
    SNAPSHOT_HAS_ASSISTANT = 1U << 4,
    SNAPSHOT_HAS_MESSAGES = 1U << 5,
    SNAPSHOT_HAS_TASKS = 1U << 6,
    SNAPSHOT_HAS_OUTPUTS = 1U << 7,
};

#define SNAPSHOT_REQUIRED_FIELDS \
    (SNAPSHOT_HAS_VERSION | SNAPSHOT_HAS_REQUEST_ID | SNAPSHOT_HAS_CURSOR | \
     SNAPSHOT_HAS_FRESHNESS | SNAPSHOT_HAS_ASSISTANT | SNAPSHOT_HAS_MESSAGES | \
     SNAPSHOT_HAS_TASKS | SNAPSHOT_HAS_OUTPUTS)

static void set_error(json_reader_t *reader, wb_protocol_result_t error)
{
    if (reader->error == WB_PROTOCOL_OK) {
        reader->error = error;
    }
}

static void skip_whitespace(json_reader_t *reader)
{
    while (reader->current < reader->end &&
           isspace((unsigned char)*reader->current) != 0) {
        ++reader->current;
    }
}

static bool consume(json_reader_t *reader, char expected)
{
    skip_whitespace(reader);
    if (reader->current >= reader->end || *reader->current != expected) {
        set_error(reader, WB_PROTOCOL_ERR_INVALID_JSON);
        return false;
    }
    ++reader->current;
    return true;
}

static bool require_type(json_reader_t *reader, char expected)
{
    skip_whitespace(reader);
    if (reader->current >= reader->end) {
        set_error(reader, WB_PROTOCOL_ERR_INVALID_JSON);
        return false;
    }
    if (*reader->current != expected) {
        set_error(reader, WB_PROTOCOL_ERR_TYPE);
        return false;
    }
    ++reader->current;
    return true;
}

static int hex_digit(char character)
{
    if (character >= '0' && character <= '9') {
        return character - '0';
    }
    if (character >= 'a' && character <= 'f') {
        return character - 'a' + 10;
    }
    if (character >= 'A' && character <= 'F') {
        return character - 'A' + 10;
    }
    return -1;
}

static bool parse_hex_quad(json_reader_t *reader, uint32_t *value)
{
    unsigned index;
    uint32_t result = 0U;

    if ((size_t)(reader->end - reader->current) < 4U) {
        set_error(reader, WB_PROTOCOL_ERR_INVALID_JSON);
        return false;
    }
    for (index = 0U; index < 4U; ++index) {
        int digit = hex_digit(reader->current[index]);

        if (digit < 0) {
            set_error(reader, WB_PROTOCOL_ERR_INVALID_JSON);
            return false;
        }
        result = (result << 4) | (uint32_t)digit;
    }
    reader->current += 4;
    *value = result;
    return true;
}

static size_t encode_utf8(uint32_t codepoint, unsigned char encoded[4])
{
    if (codepoint <= 0x7FU) {
        encoded[0] = (unsigned char)codepoint;
        return 1U;
    }
    if (codepoint <= 0x7FFU) {
        encoded[0] = (unsigned char)(0xC0U | (codepoint >> 6));
        encoded[1] = (unsigned char)(0x80U | (codepoint & 0x3FU));
        return 2U;
    }
    if (codepoint <= 0xFFFFU) {
        encoded[0] = (unsigned char)(0xE0U | (codepoint >> 12));
        encoded[1] = (unsigned char)(0x80U | ((codepoint >> 6) & 0x3FU));
        encoded[2] = (unsigned char)(0x80U | (codepoint & 0x3FU));
        return 3U;
    }
    encoded[0] = (unsigned char)(0xF0U | (codepoint >> 18));
    encoded[1] = (unsigned char)(0x80U | ((codepoint >> 12) & 0x3FU));
    encoded[2] = (unsigned char)(0x80U | ((codepoint >> 6) & 0x3FU));
    encoded[3] = (unsigned char)(0x80U | (codepoint & 0x3FU));
    return 4U;
}

static bool raw_utf8_sequence(json_reader_t *reader,
                              unsigned char encoded[4],
                              size_t *encoded_length)
{
    const unsigned char *bytes = (const unsigned char *)reader->current;
    size_t remaining = (size_t)(reader->end - reader->current);
    size_t length;
    uint32_t codepoint;
    size_t index;

    if (bytes[0] >= 0xC2U && bytes[0] <= 0xDFU) {
        length = 2U;
        codepoint = bytes[0] & 0x1FU;
    } else if (bytes[0] >= 0xE0U && bytes[0] <= 0xEFU) {
        length = 3U;
        codepoint = bytes[0] & 0x0FU;
    } else if (bytes[0] >= 0xF0U && bytes[0] <= 0xF4U) {
        length = 4U;
        codepoint = bytes[0] & 0x07U;
    } else {
        set_error(reader, WB_PROTOCOL_ERR_INVALID_JSON);
        return false;
    }
    if (remaining < length) {
        set_error(reader, WB_PROTOCOL_ERR_INVALID_JSON);
        return false;
    }
    for (index = 1U; index < length; ++index) {
        if ((bytes[index] & 0xC0U) != 0x80U) {
            set_error(reader, WB_PROTOCOL_ERR_INVALID_JSON);
            return false;
        }
        codepoint = (codepoint << 6) | (bytes[index] & 0x3FU);
    }
    if ((length == 3U && codepoint < 0x800U) ||
        (length == 4U && codepoint < 0x10000U) ||
        (codepoint >= 0xD800U && codepoint <= 0xDFFFU) ||
        codepoint > 0x10FFFFU) {
        set_error(reader, WB_PROTOCOL_ERR_INVALID_JSON);
        return false;
    }
    memcpy(encoded, bytes, length);
    reader->current += length;
    *encoded_length = length;
    return true;
}

static bool append_decoded(json_reader_t *reader,
                           char *destination,
                           size_t capacity,
                           size_t *written,
                           bool *saturated,
                           bool strict_limit,
                           const unsigned char *encoded,
                           size_t encoded_length)
{
    if (destination == NULL || *saturated) {
        return true;
    }
    if (*written + encoded_length >= capacity) {
        if (strict_limit) {
            set_error(reader, WB_PROTOCOL_ERR_LIMIT);
            return false;
        }
        *saturated = true;
        return true;
    }
    memcpy(&destination[*written], encoded, encoded_length);
    *written += encoded_length;
    return true;
}

static bool parse_json_string(json_reader_t *reader,
                              char *destination,
                              size_t capacity,
                              bool strict_limit,
                              bool field_value)
{
    size_t written = 0U;
    bool saturated = false;

    skip_whitespace(reader);
    if (reader->current >= reader->end) {
        set_error(reader, WB_PROTOCOL_ERR_INVALID_JSON);
        return false;
    }
    if (*reader->current != '"') {
        set_error(reader, field_value ? WB_PROTOCOL_ERR_TYPE : WB_PROTOCOL_ERR_INVALID_JSON);
        return false;
    }
    ++reader->current;
    if (destination != NULL && capacity == 0U) {
        set_error(reader, WB_PROTOCOL_ERR_ARGUMENT);
        return false;
    }

    while (reader->current < reader->end) {
        unsigned char encoded[4];
        size_t encoded_length = 1U;
        unsigned char byte = (unsigned char)*reader->current++;

        if (byte == '"') {
            if (destination != NULL) {
                destination[written] = '\0';
            }
            return true;
        }
        if (byte < 0x20U) {
            set_error(reader, WB_PROTOCOL_ERR_INVALID_JSON);
            return false;
        }
        if (byte == '\\') {
            uint32_t codepoint;

            if (reader->current >= reader->end) {
                set_error(reader, WB_PROTOCOL_ERR_INVALID_JSON);
                return false;
            }
            byte = (unsigned char)*reader->current++;
            switch (byte) {
            case '"':
            case '\\':
            case '/':
                encoded[0] = byte;
                break;
            case 'b':
                encoded[0] = '\b';
                break;
            case 'f':
                encoded[0] = '\f';
                break;
            case 'n':
                encoded[0] = '\n';
                break;
            case 'r':
                encoded[0] = '\r';
                break;
            case 't':
                encoded[0] = '\t';
                break;
            case 'u':
                if (!parse_hex_quad(reader, &codepoint)) {
                    return false;
                }
                if (codepoint >= 0xD800U && codepoint <= 0xDBFFU) {
                    uint32_t low_surrogate;

                    if ((size_t)(reader->end - reader->current) < 6U ||
                        reader->current[0] != '\\' || reader->current[1] != 'u') {
                        set_error(reader, WB_PROTOCOL_ERR_INVALID_JSON);
                        return false;
                    }
                    reader->current += 2;
                    if (!parse_hex_quad(reader, &low_surrogate) ||
                        low_surrogate < 0xDC00U || low_surrogate > 0xDFFFU) {
                        set_error(reader, WB_PROTOCOL_ERR_INVALID_JSON);
                        return false;
                    }
                    codepoint = UINT32_C(0x10000) +
                        ((codepoint - UINT32_C(0xD800)) << 10) +
                        (low_surrogate - UINT32_C(0xDC00));
                } else if (codepoint >= 0xDC00U && codepoint <= 0xDFFFU) {
                    set_error(reader, WB_PROTOCOL_ERR_INVALID_JSON);
                    return false;
                }
                if (codepoint == 0U && destination != NULL) {
                    set_error(reader, WB_PROTOCOL_ERR_INVALID_JSON);
                    return false;
                }
                encoded_length = encode_utf8(codepoint, encoded);
                break;
            default:
                set_error(reader, WB_PROTOCOL_ERR_INVALID_JSON);
                return false;
            }
        } else if (byte < 0x80U) {
            encoded[0] = byte;
        } else {
            --reader->current;
            if (!raw_utf8_sequence(reader, encoded, &encoded_length)) {
                return false;
            }
        }

        if (!append_decoded(reader, destination, capacity, &written, &saturated,
                            strict_limit, encoded, encoded_length)) {
            return false;
        }
    }
    set_error(reader, WB_PROTOCOL_ERR_INVALID_JSON);
    return false;
}

static bool parse_boolean(json_reader_t *reader, bool *value)
{
    size_t remaining;

    skip_whitespace(reader);
    remaining = (size_t)(reader->end - reader->current);
    if (remaining >= 4U && memcmp(reader->current, "true", 4U) == 0) {
        reader->current += 4;
        *value = true;
        return true;
    }
    if (remaining >= 5U && memcmp(reader->current, "false", 5U) == 0) {
        reader->current += 5;
        *value = false;
        return true;
    }
    set_error(reader, WB_PROTOCOL_ERR_TYPE);
    return false;
}

static bool parse_unsigned(json_reader_t *reader, unsigned *value)
{
    unsigned result = 0U;
    bool first = true;

    skip_whitespace(reader);
    if (reader->current >= reader->end ||
        !isdigit((unsigned char)*reader->current)) {
        set_error(reader, WB_PROTOCOL_ERR_TYPE);
        return false;
    }
    if (*reader->current == '0' && reader->current + 1 < reader->end &&
        isdigit((unsigned char)reader->current[1])) {
        set_error(reader, WB_PROTOCOL_ERR_INVALID_JSON);
        return false;
    }
    while (reader->current < reader->end &&
           isdigit((unsigned char)*reader->current)) {
        unsigned digit = (unsigned)(*reader->current - '0');

        if (!first && result > (UINT_MAX - digit) / 10U) {
            set_error(reader, WB_PROTOCOL_ERR_LIMIT);
            return false;
        }
        result = result * 10U + digit;
        first = false;
        ++reader->current;
    }
    if (reader->current < reader->end &&
        (*reader->current == '.' || *reader->current == 'e' ||
         *reader->current == 'E')) {
        set_error(reader, WB_PROTOCOL_ERR_TYPE);
        return false;
    }
    *value = result;
    return true;
}

static bool skip_value(json_reader_t *reader, unsigned depth);

static bool skip_number(json_reader_t *reader)
{
    if (reader->current < reader->end && *reader->current == '-') {
        ++reader->current;
    }
    if (reader->current >= reader->end) {
        set_error(reader, WB_PROTOCOL_ERR_INVALID_JSON);
        return false;
    }
    if (*reader->current == '0') {
        ++reader->current;
        if (reader->current < reader->end &&
            isdigit((unsigned char)*reader->current)) {
            set_error(reader, WB_PROTOCOL_ERR_INVALID_JSON);
            return false;
        }
    } else if (isdigit((unsigned char)*reader->current)) {
        do {
            ++reader->current;
        } while (reader->current < reader->end &&
                 isdigit((unsigned char)*reader->current));
    } else {
        set_error(reader, WB_PROTOCOL_ERR_INVALID_JSON);
        return false;
    }
    if (reader->current < reader->end && *reader->current == '.') {
        ++reader->current;
        if (reader->current >= reader->end ||
            !isdigit((unsigned char)*reader->current)) {
            set_error(reader, WB_PROTOCOL_ERR_INVALID_JSON);
            return false;
        }
        do {
            ++reader->current;
        } while (reader->current < reader->end &&
                 isdigit((unsigned char)*reader->current));
    }
    if (reader->current < reader->end &&
        (*reader->current == 'e' || *reader->current == 'E')) {
        ++reader->current;
        if (reader->current < reader->end &&
            (*reader->current == '+' || *reader->current == '-')) {
            ++reader->current;
        }
        if (reader->current >= reader->end ||
            !isdigit((unsigned char)*reader->current)) {
            set_error(reader, WB_PROTOCOL_ERR_INVALID_JSON);
            return false;
        }
        do {
            ++reader->current;
        } while (reader->current < reader->end &&
                 isdigit((unsigned char)*reader->current));
    }
    return true;
}

static bool skip_object(json_reader_t *reader, unsigned depth)
{
    if (!consume(reader, '{')) {
        return false;
    }
    skip_whitespace(reader);
    if (reader->current < reader->end && *reader->current == '}') {
        ++reader->current;
        return true;
    }
    for (;;) {
        if (!parse_json_string(reader, NULL, 0U, false, false) ||
            !consume(reader, ':') || !skip_value(reader, depth + 1U)) {
            return false;
        }
        skip_whitespace(reader);
        if (reader->current < reader->end && *reader->current == '}') {
            ++reader->current;
            return true;
        }
        if (!consume(reader, ',')) {
            return false;
        }
    }
}

static bool skip_array(json_reader_t *reader, unsigned depth)
{
    if (!consume(reader, '[')) {
        return false;
    }
    skip_whitespace(reader);
    if (reader->current < reader->end && *reader->current == ']') {
        ++reader->current;
        return true;
    }
    for (;;) {
        if (!skip_value(reader, depth + 1U)) {
            return false;
        }
        skip_whitespace(reader);
        if (reader->current < reader->end && *reader->current == ']') {
            ++reader->current;
            return true;
        }
        if (!consume(reader, ',')) {
            return false;
        }
    }
}

static bool skip_value(json_reader_t *reader, unsigned depth)
{
    size_t remaining;

    if (depth > 16U) {
        set_error(reader, WB_PROTOCOL_ERR_LIMIT);
        return false;
    }
    skip_whitespace(reader);
    if (reader->current >= reader->end) {
        set_error(reader, WB_PROTOCOL_ERR_INVALID_JSON);
        return false;
    }
    if (*reader->current == '"') {
        return parse_json_string(reader, NULL, 0U, false, false);
    }
    if (*reader->current == '{') {
        return skip_object(reader, depth);
    }
    if (*reader->current == '[') {
        return skip_array(reader, depth);
    }
    remaining = (size_t)(reader->end - reader->current);
    if (remaining >= 4U && memcmp(reader->current, "true", 4U) == 0) {
        reader->current += 4;
        return true;
    }
    if (remaining >= 5U && memcmp(reader->current, "false", 5U) == 0) {
        reader->current += 5;
        return true;
    }
    if (remaining >= 4U && memcmp(reader->current, "null", 4U) == 0) {
        reader->current += 4;
        return true;
    }
    return skip_number(reader);
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

static bool parse_message(json_reader_t *reader, wb_message_t *message)
{
    bool has_id = false;

    if (!require_type(reader, '{')) {
        return false;
    }
    skip_whitespace(reader);
    if (reader->current < reader->end && *reader->current == '}') {
        ++reader->current;
        set_error(reader, WB_PROTOCOL_ERR_MISSING_FIELD);
        return false;
    }
    for (;;) {
        char key[32];

        if (!parse_json_string(reader, key, sizeof(key), false, false) ||
            !consume(reader, ':')) {
            return false;
        }
        if (strcmp(key, "id") == 0 || strcmp(key, "message_id") == 0) {
            if (!parse_json_string(reader, message->id, sizeof(message->id), true, true)) {
                return false;
            }
            has_id = message->id[0] != '\0';
        } else if (strcmp(key, "title") == 0 || strcmp(key, "sender") == 0) {
            if (!parse_json_string(reader, message->title, sizeof(message->title), false, true)) {
                return false;
            }
        } else if (strcmp(key, "preview") == 0) {
            if (!parse_json_string(reader, message->preview, sizeof(message->preview), false,
                                   true)) {
                return false;
            }
        } else if (strcmp(key, "role") == 0 || strcmp(key, "sender_role") == 0) {
            char role[32];

            if (!parse_json_string(reader, role, sizeof(role), false, true)) {
                return false;
            }
            message->role = normalize_role(role);
        } else if (strcmp(key, "unread") == 0) {
            if (!parse_boolean(reader, &message->unread)) {
                return false;
            }
        } else if (!skip_value(reader, 0U)) {
            return false;
        }

        skip_whitespace(reader);
        if (reader->current < reader->end && *reader->current == '}') {
            ++reader->current;
            break;
        }
        if (!consume(reader, ',')) {
            return false;
        }
    }
    if (!has_id) {
        set_error(reader, WB_PROTOCOL_ERR_MISSING_FIELD);
        return false;
    }
    return true;
}

static bool parse_task(json_reader_t *reader, wb_task_t *task)
{
    bool has_id = false;
    bool has_status = false;

    if (!require_type(reader, '{')) {
        return false;
    }
    skip_whitespace(reader);
    if (reader->current < reader->end && *reader->current == '}') {
        ++reader->current;
        set_error(reader, WB_PROTOCOL_ERR_MISSING_FIELD);
        return false;
    }
    for (;;) {
        char key[32];

        if (!parse_json_string(reader, key, sizeof(key), false, false) ||
            !consume(reader, ':')) {
            return false;
        }
        if (strcmp(key, "id") == 0 || strcmp(key, "task_id") == 0) {
            if (!parse_json_string(reader, task->id, sizeof(task->id), true, true)) {
                return false;
            }
            has_id = task->id[0] != '\0';
        } else if (strcmp(key, "title") == 0) {
            if (!parse_json_string(reader, task->title, sizeof(task->title), false, true)) {
                return false;
            }
        } else if (strcmp(key, "preview") == 0 || strcmp(key, "description") == 0) {
            if (!parse_json_string(reader, task->preview, sizeof(task->preview), false, true)) {
                return false;
            }
        } else if (strcmp(key, "status") == 0) {
            char status[32];

            if (!parse_json_string(reader, status, sizeof(status), false, true)) {
                return false;
            }
            task->status = wb_task_status_normalize(status);
            has_status = true;
        } else if (!skip_value(reader, 0U)) {
            return false;
        }

        skip_whitespace(reader);
        if (reader->current < reader->end && *reader->current == '}') {
            ++reader->current;
            break;
        }
        if (!consume(reader, ',')) {
            return false;
        }
    }
    if (!has_id || !has_status) {
        set_error(reader, WB_PROTOCOL_ERR_MISSING_FIELD);
        return false;
    }
    return true;
}

static bool parse_output(json_reader_t *reader, wb_output_t *output)
{
    bool has_id = false;

    if (!require_type(reader, '{')) {
        return false;
    }
    skip_whitespace(reader);
    if (reader->current < reader->end && *reader->current == '}') {
        ++reader->current;
        set_error(reader, WB_PROTOCOL_ERR_MISSING_FIELD);
        return false;
    }
    for (;;) {
        char key[32];

        if (!parse_json_string(reader, key, sizeof(key), false, false) ||
            !consume(reader, ':')) {
            return false;
        }
        if (strcmp(key, "id") == 0 || strcmp(key, "output_id") == 0 ||
            strcmp(key, "artifact_id") == 0) {
            if (!parse_json_string(reader, output->id, sizeof(output->id), true, true)) {
                return false;
            }
            has_id = output->id[0] != '\0';
        } else if (strcmp(key, "task_id") == 0) {
            if (!parse_json_string(reader, output->task_id, sizeof(output->task_id), true,
                                   true)) {
                return false;
            }
        } else if (strcmp(key, "title") == 0) {
            if (!parse_json_string(reader, output->title, sizeof(output->title), false, true)) {
                return false;
            }
        } else if (strcmp(key, "preview") == 0 || strcmp(key, "description") == 0) {
            if (!parse_json_string(reader, output->preview, sizeof(output->preview), false,
                                   true)) {
                return false;
            }
        } else if (strcmp(key, "kind") == 0 || strcmp(key, "type") == 0) {
            char kind[32];

            if (!parse_json_string(reader, kind, sizeof(kind), false, true)) {
                return false;
            }
            output->kind = normalize_output_kind(kind);
        } else if (!skip_value(reader, 0U)) {
            return false;
        }

        skip_whitespace(reader);
        if (reader->current < reader->end && *reader->current == '}') {
            ++reader->current;
            break;
        }
        if (!consume(reader, ',')) {
            return false;
        }
    }
    if (!has_id) {
        set_error(reader, WB_PROTOCOL_ERR_MISSING_FIELD);
        return false;
    }
    return true;
}

typedef bool (*parse_item_fn)(json_reader_t *reader, void *item);

static bool parse_message_item(json_reader_t *reader, void *item)
{
    return parse_message(reader, item);
}

static bool parse_task_item(json_reader_t *reader, void *item)
{
    return parse_task(reader, item);
}

static bool parse_output_item(json_reader_t *reader, void *item)
{
    return parse_output(reader, item);
}

static bool parse_bounded_array(json_reader_t *reader,
                                void *items,
                                size_t item_size,
                                size_t *item_count,
                                parse_item_fn parse_item)
{
    size_t total = 0U;

    if (!require_type(reader, '[')) {
        return false;
    }
    skip_whitespace(reader);
    if (reader->current < reader->end && *reader->current == ']') {
        ++reader->current;
        *item_count = 0U;
        return true;
    }
    for (;;) {
        if (total < WB_MAX_ITEMS) {
            void *item = (unsigned char *)items + total * item_size;

            if (!parse_item(reader, item)) {
                return false;
            }
        } else if (!skip_value(reader, 0U)) {
            return false;
        }
        ++total;
        skip_whitespace(reader);
        if (reader->current < reader->end && *reader->current == ']') {
            ++reader->current;
            *item_count = total < WB_MAX_ITEMS ? total : WB_MAX_ITEMS;
            return true;
        }
        if (!consume(reader, ',')) {
            return false;
        }
    }
}

static bool parse_snapshot_object(json_reader_t *reader,
                                  wb_snapshot_t *snapshot,
                                  unsigned *fields)
{
    if (!require_type(reader, '{')) {
        return false;
    }
    skip_whitespace(reader);
    if (reader->current < reader->end && *reader->current == '}') {
        ++reader->current;
        return true;
    }
    for (;;) {
        char key[32];

        if (!parse_json_string(reader, key, sizeof(key), false, false) ||
            !consume(reader, ':')) {
            return false;
        }
        if (strcmp(key, "version") == 0) {
            if (!parse_unsigned(reader, &snapshot->version)) {
                return false;
            }
            *fields |= SNAPSHOT_HAS_VERSION;
            if (snapshot->version != 1U) {
                set_error(reader, WB_PROTOCOL_ERR_UNSUPPORTED_VERSION);
                return false;
            }
        } else if (strcmp(key, "request_id") == 0) {
            if (!parse_json_string(reader, snapshot->request_id,
                                   sizeof(snapshot->request_id), true, true)) {
                return false;
            }
            *fields |= SNAPSHOT_HAS_REQUEST_ID;
        } else if (strcmp(key, "cursor") == 0) {
            if (!parse_json_string(reader, snapshot->cursor,
                                   sizeof(snapshot->cursor), true, true)) {
                return false;
            }
            *fields |= SNAPSHOT_HAS_CURSOR;
        } else if (strcmp(key, "fresh") == 0) {
            if (!parse_boolean(reader, &snapshot->fresh)) {
                return false;
            }
            *fields |= SNAPSHOT_HAS_FRESHNESS;
        } else if (strcmp(key, "stale") == 0) {
            bool stale;

            if (!parse_boolean(reader, &stale)) {
                return false;
            }
            snapshot->fresh = !stale;
            *fields |= SNAPSHOT_HAS_FRESHNESS;
        } else if (strcmp(key, "assistant_available") == 0) {
            if (!parse_boolean(reader, &snapshot->assistant_available)) {
                return false;
            }
            *fields |= SNAPSHOT_HAS_ASSISTANT;
        } else if (strcmp(key, "unread_count") == 0) {
            if (!parse_unsigned(reader, &snapshot->unread_count)) {
                return false;
            }
        } else if (strcmp(key, "active_task_count") == 0) {
            if (!parse_unsigned(reader, &snapshot->active_task_count)) {
                return false;
            }
        } else if (strcmp(key, "messages") == 0) {
            if (!parse_bounded_array(reader, snapshot->messages,
                                     sizeof(snapshot->messages[0]),
                                     &snapshot->message_count, parse_message_item)) {
                return false;
            }
            *fields |= SNAPSHOT_HAS_MESSAGES;
        } else if (strcmp(key, "tasks") == 0) {
            if (!parse_bounded_array(reader, snapshot->tasks,
                                     sizeof(snapshot->tasks[0]),
                                     &snapshot->task_count, parse_task_item)) {
                return false;
            }
            *fields |= SNAPSHOT_HAS_TASKS;
        } else if (strcmp(key, "outputs") == 0 || strcmp(key, "artifacts") == 0) {
            if (!parse_bounded_array(reader, snapshot->outputs,
                                     sizeof(snapshot->outputs[0]),
                                     &snapshot->output_count, parse_output_item)) {
                return false;
            }
            *fields |= SNAPSHOT_HAS_OUTPUTS;
        } else if (!skip_value(reader, 0U)) {
            return false;
        }

        skip_whitespace(reader);
        if (reader->current < reader->end && *reader->current == '}') {
            ++reader->current;
            return true;
        }
        if (!consume(reader, ',')) {
            return false;
        }
    }
}

wb_protocol_result_t wb_protocol_parse_snapshot(const char *json,
                                                size_t json_length,
                                                wb_snapshot_t *snapshot)
{
    json_reader_t reader;
    wb_snapshot_t parsed = { 0 };
    unsigned fields = 0U;

    if (json == NULL || snapshot == NULL || json_length == 0U) {
        return WB_PROTOCOL_ERR_ARGUMENT;
    }
    if (json_length > WB_SNAPSHOT_MAX_BYTES) {
        return WB_PROTOCOL_ERR_LIMIT;
    }

    reader.current = json;
    reader.end = json + json_length;
    reader.error = WB_PROTOCOL_OK;
    if (!parse_snapshot_object(&reader, &parsed, &fields)) {
        return reader.error == WB_PROTOCOL_OK ? WB_PROTOCOL_ERR_INVALID_JSON : reader.error;
    }
    skip_whitespace(&reader);
    if (reader.current != reader.end) {
        return WB_PROTOCOL_ERR_INVALID_JSON;
    }
    if ((fields & SNAPSHOT_REQUIRED_FIELDS) != SNAPSHOT_REQUIRED_FIELDS) {
        return WB_PROTOCOL_ERR_MISSING_FIELD;
    }
    *snapshot = parsed;
    return WB_PROTOCOL_OK;
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
    if (action->type == WB_ACTION_REPLY || action->type == WB_ACTION_TASK_FOLLOWUP) {
        if (!bounded_string_length(action->target_id, WB_ID_MAX_BYTES, &target_length)) {
            return WB_PROTOCOL_ERR_LIMIT;
        }
        if (target_length == 0U) {
            return WB_PROTOCOL_ERR_ARGUMENT;
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
    default:
        return "UNKNOWN";
    }
}
