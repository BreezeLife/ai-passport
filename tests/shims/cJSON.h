#ifndef WORKBUDDY_TEST_CJSON_H
#define WORKBUDDY_TEST_CJSON_H

/*
 * Minimal host-only cJSON compatibility shim. Firmware uses ESP-IDF's cJSON;
 * this header keeps ordinary-cc tests dependency-free while exercising the
 * same allocation hooks and DOM-facing protocol code.
 */

#include <ctype.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef int cJSON_bool;

enum {
    cJSON_Invalid = 0,
    cJSON_False = 1,
    cJSON_True = 2,
    cJSON_NULL = 4,
    cJSON_Number = 8,
    cJSON_String = 16,
    cJSON_Array = 32,
    cJSON_Object = 64,
};

typedef struct cJSON {
    struct cJSON *next;
    struct cJSON *prev;
    struct cJSON *child;
    int type;
    char *valuestring;
    int valueint;
    double valuedouble;
    char *string;
} cJSON;

typedef struct cJSON_Hooks {
    void *(*malloc_fn)(size_t size);
    void (*free_fn)(void *pointer);
} cJSON_Hooks;

typedef struct {
    const char *current;
    const char *end;
    cJSON_bool failed;
} workbuddy_cjson_reader_t;

static cJSON_Hooks s_workbuddy_cjson_hooks = { malloc, free };

static inline void cJSON_InitHooks(cJSON_Hooks *hooks)
{
    if (hooks == NULL) {
        s_workbuddy_cjson_hooks.malloc_fn = malloc;
        s_workbuddy_cjson_hooks.free_fn = free;
        return;
    }
    s_workbuddy_cjson_hooks.malloc_fn = hooks->malloc_fn != NULL
        ? hooks->malloc_fn : malloc;
    s_workbuddy_cjson_hooks.free_fn = hooks->free_fn != NULL
        ? hooks->free_fn : free;
}

static inline void workbuddy_cjson_skip_space(workbuddy_cjson_reader_t *reader)
{
    while (reader->current < reader->end &&
           isspace((unsigned char)*reader->current) != 0) {
        ++reader->current;
    }
}

static inline cJSON *workbuddy_cjson_new(int type)
{
    cJSON *item = s_workbuddy_cjson_hooks.malloc_fn(sizeof(*item));

    if (item != NULL) {
        memset(item, 0, sizeof(*item));
        item->type = type;
    }
    return item;
}

static inline void cJSON_Delete(cJSON *item)
{
    while (item != NULL) {
        cJSON *next = item->next;

        if (item->child != NULL) {
            cJSON_Delete(item->child);
        }
        if (item->valuestring != NULL) {
            s_workbuddy_cjson_hooks.free_fn(item->valuestring);
        }
        if (item->string != NULL) {
            s_workbuddy_cjson_hooks.free_fn(item->string);
        }
        s_workbuddy_cjson_hooks.free_fn(item);
        item = next;
    }
}

static inline int workbuddy_cjson_hex(char character)
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

static inline cJSON_bool workbuddy_cjson_hex_quad(workbuddy_cjson_reader_t *reader,
                                                  uint32_t *value)
{
    uint32_t result = 0U;
    unsigned index;

    if ((size_t)(reader->end - reader->current) < 4U) {
        reader->failed = 1;
        return 0;
    }
    for (index = 0U; index < 4U; ++index) {
        int digit = workbuddy_cjson_hex(reader->current[index]);

        if (digit < 0) {
            reader->failed = 1;
            return 0;
        }
        result = (result << 4) | (uint32_t)digit;
    }
    reader->current += 4;
    *value = result;
    return 1;
}

static inline size_t workbuddy_cjson_encode_utf8(uint32_t codepoint,
                                                 char output[4])
{
    if (codepoint <= 0x7FU) {
        output[0] = (char)codepoint;
        return 1U;
    }
    if (codepoint <= 0x7FFU) {
        output[0] = (char)(0xC0U | (codepoint >> 6));
        output[1] = (char)(0x80U | (codepoint & 0x3FU));
        return 2U;
    }
    if (codepoint <= 0xFFFFU) {
        output[0] = (char)(0xE0U | (codepoint >> 12));
        output[1] = (char)(0x80U | ((codepoint >> 6) & 0x3FU));
        output[2] = (char)(0x80U | (codepoint & 0x3FU));
        return 3U;
    }
    output[0] = (char)(0xF0U | (codepoint >> 18));
    output[1] = (char)(0x80U | ((codepoint >> 12) & 0x3FU));
    output[2] = (char)(0x80U | ((codepoint >> 6) & 0x3FU));
    output[3] = (char)(0x80U | (codepoint & 0x3FU));
    return 4U;
}

static inline char *workbuddy_cjson_parse_string(workbuddy_cjson_reader_t *reader)
{
    char *output;
    size_t written = 0U;

    workbuddy_cjson_skip_space(reader);
    if (reader->current >= reader->end || *reader->current != '"') {
        reader->failed = 1;
        return NULL;
    }
    ++reader->current;
    output = s_workbuddy_cjson_hooks.malloc_fn(
        (size_t)(reader->end - reader->current) + 1U);
    if (output == NULL) {
        reader->failed = 1;
        return NULL;
    }

    while (reader->current < reader->end) {
        unsigned char byte = (unsigned char)*reader->current++;

        if (byte == '"') {
            output[written] = '\0';
            return output;
        }
        if (byte < 0x20U) {
            break;
        }
        if (byte != '\\') {
            output[written++] = (char)byte;
            continue;
        }
        if (reader->current >= reader->end) {
            break;
        }
        byte = (unsigned char)*reader->current++;
        switch (byte) {
        case '"':
        case '\\':
        case '/':
            output[written++] = (char)byte;
            break;
        case 'b':
            output[written++] = '\b';
            break;
        case 'f':
            output[written++] = '\f';
            break;
        case 'n':
            output[written++] = '\n';
            break;
        case 'r':
            output[written++] = '\r';
            break;
        case 't':
            output[written++] = '\t';
            break;
        case 'u': {
            uint32_t codepoint;
            char encoded[4];
            size_t encoded_length;

            if (!workbuddy_cjson_hex_quad(reader, &codepoint)) {
                s_workbuddy_cjson_hooks.free_fn(output);
                return NULL;
            }
            if (codepoint >= 0xD800U && codepoint <= 0xDBFFU) {
                uint32_t low;

                if ((size_t)(reader->end - reader->current) < 6U ||
                    reader->current[0] != '\\' || reader->current[1] != 'u') {
                    s_workbuddy_cjson_hooks.free_fn(output);
                    reader->failed = 1;
                    return NULL;
                }
                reader->current += 2;
                if (!workbuddy_cjson_hex_quad(reader, &low) ||
                    low < 0xDC00U || low > 0xDFFFU) {
                    s_workbuddy_cjson_hooks.free_fn(output);
                    reader->failed = 1;
                    return NULL;
                }
                codepoint = UINT32_C(0x10000) +
                    ((codepoint - UINT32_C(0xD800)) << 10) +
                    (low - UINT32_C(0xDC00));
            } else if ((codepoint >= 0xDC00U && codepoint <= 0xDFFFU) ||
                       codepoint == 0U) {
                s_workbuddy_cjson_hooks.free_fn(output);
                reader->failed = 1;
                return NULL;
            }
            encoded_length = workbuddy_cjson_encode_utf8(codepoint, encoded);
            memcpy(&output[written], encoded, encoded_length);
            written += encoded_length;
            break;
        }
        default:
            s_workbuddy_cjson_hooks.free_fn(output);
            reader->failed = 1;
            return NULL;
        }
    }

    s_workbuddy_cjson_hooks.free_fn(output);
    reader->failed = 1;
    return NULL;
}

static inline cJSON_bool workbuddy_cjson_match(workbuddy_cjson_reader_t *reader,
                                               const char *literal)
{
    size_t length = strlen(literal);

    if ((size_t)(reader->end - reader->current) < length ||
        memcmp(reader->current, literal, length) != 0) {
        return 0;
    }
    reader->current += length;
    return 1;
}

static inline cJSON *workbuddy_cjson_parse_value(workbuddy_cjson_reader_t *reader,
                                                 unsigned depth);

static inline void workbuddy_cjson_append_child(cJSON *parent, cJSON *child)
{
    cJSON *tail;

    if (parent->child == NULL) {
        parent->child = child;
        return;
    }
    tail = parent->child;
    while (tail->next != NULL) {
        tail = tail->next;
    }
    tail->next = child;
    child->prev = tail;
}

static inline cJSON *workbuddy_cjson_parse_object(workbuddy_cjson_reader_t *reader,
                                                  unsigned depth)
{
    cJSON *object = workbuddy_cjson_new(cJSON_Object);

    if (object == NULL) {
        reader->failed = 1;
        return NULL;
    }
    ++reader->current;
    workbuddy_cjson_skip_space(reader);
    if (reader->current < reader->end && *reader->current == '}') {
        ++reader->current;
        return object;
    }

    for (;;) {
        char *key = workbuddy_cjson_parse_string(reader);
        cJSON *child;

        if (key == NULL) {
            cJSON_Delete(object);
            return NULL;
        }
        workbuddy_cjson_skip_space(reader);
        if (reader->current >= reader->end || *reader->current != ':') {
            s_workbuddy_cjson_hooks.free_fn(key);
            cJSON_Delete(object);
            reader->failed = 1;
            return NULL;
        }
        ++reader->current;
        child = workbuddy_cjson_parse_value(reader, depth + 1U);
        if (child == NULL) {
            s_workbuddy_cjson_hooks.free_fn(key);
            cJSON_Delete(object);
            return NULL;
        }
        child->string = key;
        workbuddy_cjson_append_child(object, child);

        workbuddy_cjson_skip_space(reader);
        if (reader->current < reader->end && *reader->current == '}') {
            ++reader->current;
            return object;
        }
        if (reader->current >= reader->end || *reader->current != ',') {
            cJSON_Delete(object);
            reader->failed = 1;
            return NULL;
        }
        ++reader->current;
    }
}

static inline cJSON *workbuddy_cjson_parse_array(workbuddy_cjson_reader_t *reader,
                                                 unsigned depth)
{
    cJSON *array = workbuddy_cjson_new(cJSON_Array);

    if (array == NULL) {
        reader->failed = 1;
        return NULL;
    }
    ++reader->current;
    workbuddy_cjson_skip_space(reader);
    if (reader->current < reader->end && *reader->current == ']') {
        ++reader->current;
        return array;
    }
    for (;;) {
        cJSON *child = workbuddy_cjson_parse_value(reader, depth + 1U);

        if (child == NULL) {
            cJSON_Delete(array);
            return NULL;
        }
        workbuddy_cjson_append_child(array, child);
        workbuddy_cjson_skip_space(reader);
        if (reader->current < reader->end && *reader->current == ']') {
            ++reader->current;
            return array;
        }
        if (reader->current >= reader->end || *reader->current != ',') {
            cJSON_Delete(array);
            reader->failed = 1;
            return NULL;
        }
        ++reader->current;
    }
}

static inline cJSON *workbuddy_cjson_parse_number(workbuddy_cjson_reader_t *reader)
{
    const char *start = reader->current;
    char buffer[128];
    size_t length;
    cJSON *number;

    if (reader->current < reader->end && *reader->current == '-') {
        ++reader->current;
    }
    if (reader->current >= reader->end) {
        reader->failed = 1;
        return NULL;
    }
    if (*reader->current == '0') {
        ++reader->current;
        if (reader->current < reader->end &&
            isdigit((unsigned char)*reader->current)) {
            reader->failed = 1;
            return NULL;
        }
    } else if (isdigit((unsigned char)*reader->current) != 0) {
        do {
            ++reader->current;
        } while (reader->current < reader->end &&
                 isdigit((unsigned char)*reader->current) != 0);
    } else {
        reader->failed = 1;
        return NULL;
    }
    if (reader->current < reader->end && *reader->current == '.') {
        ++reader->current;
        if (reader->current >= reader->end ||
            isdigit((unsigned char)*reader->current) == 0) {
            reader->failed = 1;
            return NULL;
        }
        do {
            ++reader->current;
        } while (reader->current < reader->end &&
                 isdigit((unsigned char)*reader->current) != 0);
    }
    if (reader->current < reader->end &&
        (*reader->current == 'e' || *reader->current == 'E')) {
        ++reader->current;
        if (reader->current < reader->end &&
            (*reader->current == '+' || *reader->current == '-')) {
            ++reader->current;
        }
        if (reader->current >= reader->end ||
            isdigit((unsigned char)*reader->current) == 0) {
            reader->failed = 1;
            return NULL;
        }
        do {
            ++reader->current;
        } while (reader->current < reader->end &&
                 isdigit((unsigned char)*reader->current) != 0);
    }

    length = (size_t)(reader->current - start);
    if (length >= sizeof(buffer)) {
        reader->failed = 1;
        return NULL;
    }
    memcpy(buffer, start, length);
    buffer[length] = '\0';
    number = workbuddy_cjson_new(cJSON_Number);
    if (number == NULL) {
        reader->failed = 1;
        return NULL;
    }
    number->valuedouble = strtod(buffer, NULL);
    if (number->valuedouble > (double)INT32_MAX) {
        number->valueint = INT32_MAX;
    } else if (number->valuedouble < (double)INT32_MIN) {
        number->valueint = INT32_MIN;
    } else {
        number->valueint = (int)number->valuedouble;
    }
    return number;
}

static inline cJSON *workbuddy_cjson_parse_value(workbuddy_cjson_reader_t *reader,
                                                 unsigned depth)
{
    cJSON *item;

    if (depth > 64U) {
        reader->failed = 1;
        return NULL;
    }
    workbuddy_cjson_skip_space(reader);
    if (reader->current >= reader->end) {
        reader->failed = 1;
        return NULL;
    }
    if (*reader->current == '{') {
        return workbuddy_cjson_parse_object(reader, depth);
    }
    if (*reader->current == '[') {
        return workbuddy_cjson_parse_array(reader, depth);
    }
    if (*reader->current == '"') {
        char *value = workbuddy_cjson_parse_string(reader);

        if (value == NULL) {
            return NULL;
        }
        item = workbuddy_cjson_new(cJSON_String);
        if (item == NULL) {
            s_workbuddy_cjson_hooks.free_fn(value);
            reader->failed = 1;
            return NULL;
        }
        item->valuestring = value;
        return item;
    }
    if (workbuddy_cjson_match(reader, "true")) {
        return workbuddy_cjson_new(cJSON_True);
    }
    if (workbuddy_cjson_match(reader, "false")) {
        return workbuddy_cjson_new(cJSON_False);
    }
    if (workbuddy_cjson_match(reader, "null")) {
        return workbuddy_cjson_new(cJSON_NULL);
    }
    return workbuddy_cjson_parse_number(reader);
}

static inline cJSON *cJSON_ParseWithLengthOpts(const char *value,
                                               size_t buffer_length,
                                               const char **return_parse_end,
                                               cJSON_bool require_null_terminated)
{
    workbuddy_cjson_reader_t reader;
    cJSON *root;

    (void)require_null_terminated;
    if (value == NULL) {
        return NULL;
    }
    reader.current = value;
    reader.end = value + buffer_length;
    reader.failed = 0;
    root = workbuddy_cjson_parse_value(&reader, 0U);
    if (root == NULL) {
        if (return_parse_end != NULL) {
            *return_parse_end = reader.current;
        }
        return NULL;
    }
    workbuddy_cjson_skip_space(&reader);
    if (reader.current != reader.end) {
        cJSON_Delete(root);
        if (return_parse_end != NULL) {
            *return_parse_end = reader.current;
        }
        return NULL;
    }
    if (return_parse_end != NULL) {
        *return_parse_end = reader.current;
    }
    return root;
}

static inline cJSON *cJSON_GetObjectItemCaseSensitive(const cJSON *object,
                                                      const char *name)
{
    cJSON *child;

    if (object == NULL || object->type != cJSON_Object || name == NULL) {
        return NULL;
    }
    child = object->child;
    while (child != NULL) {
        if (child->string != NULL && strcmp(child->string, name) == 0) {
            return child;
        }
        child = child->next;
    }
    return NULL;
}

static inline int cJSON_GetArraySize(const cJSON *array)
{
    int count = 0;
    cJSON *child;

    if (array == NULL || array->type != cJSON_Array) {
        return 0;
    }
    child = array->child;
    while (child != NULL) {
        ++count;
        child = child->next;
    }
    return count;
}

static inline cJSON *cJSON_GetArrayItem(const cJSON *array, int index)
{
    cJSON *child;

    if (array == NULL || array->type != cJSON_Array || index < 0) {
        return NULL;
    }
    child = array->child;
    while (child != NULL && index > 0) {
        child = child->next;
        --index;
    }
    return child;
}

static inline cJSON_bool cJSON_IsObject(const cJSON *item)
{
    return item != NULL && item->type == cJSON_Object;
}

static inline cJSON_bool cJSON_IsArray(const cJSON *item)
{
    return item != NULL && item->type == cJSON_Array;
}

static inline cJSON_bool cJSON_IsString(const cJSON *item)
{
    return item != NULL && item->type == cJSON_String;
}

static inline cJSON_bool cJSON_IsNumber(const cJSON *item)
{
    return item != NULL && item->type == cJSON_Number;
}

static inline cJSON_bool cJSON_IsBool(const cJSON *item)
{
    return item != NULL && (item->type == cJSON_True || item->type == cJSON_False);
}

static inline cJSON_bool cJSON_IsTrue(const cJSON *item)
{
    return item != NULL && item->type == cJSON_True;
}

#endif
