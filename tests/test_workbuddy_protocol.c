#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "workbuddy_protocol.h"

static unsigned s_tests_run;
static unsigned s_allocation_calls;
static unsigned s_fail_allocation_at;

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, #condition); \
        return 1; \
    } \
} while (0)

#define RUN_TEST(function) do { \
    int result = function(); \
    if (result != 0) { \
        return result; \
    } \
    ++s_tests_run; \
} while (0)

static const char s_valid_snapshot[] =
    "{"
    "\"version\":1,"
    "\"request_id\":\"request-1\","
    "\"cursor\":\"cursor-9\","
    "\"fresh\":true,"
    "\"assistant_available\":true,"
    "\"unread_count\":7,"
    "\"active_task_count\":2,"
    "\"messages\":[{"
        "\"id\":\"message-1\","
        "\"title\":\"WorkBuddy\","
        "\"preview\":\"Ready \\u4f60\\u597d\","
        "\"role\":\"assistant\","
        "\"unread\":true"
    "}],"
    "\"tasks\":[{"
        "\"id\":\"task-1\","
        "\"title\":\"Launch plan\","
        "\"preview\":\"Drafting\","
        "\"status\":\"RUNNING\""
    "}],"
    "\"outputs\":[{"
        "\"id\":\"output-1\","
        "\"task_id\":\"task-1\","
        "\"title\":\"Checklist\","
        "\"preview\":\"Three steps\","
        "\"kind\":\"checklist\""
    "}]"
    "}";

static void *failing_allocate(size_t size)
{
    ++s_allocation_calls;
    if (s_allocation_calls == s_fail_allocation_at) {
        return NULL;
    }
    return malloc(size);
}

static void test_deallocate(void *pointer)
{
    free(pointer);
}

static int test_parses_version_one_snapshot(void)
{
    wb_snapshot_t snapshot;

    CHECK(wb_protocol_parse_snapshot(s_valid_snapshot, strlen(s_valid_snapshot), &snapshot) ==
          WB_PROTOCOL_OK);
    CHECK(snapshot.version == 1U);
    CHECK(strcmp(snapshot.request_id, "request-1") == 0);
    CHECK(strcmp(snapshot.cursor, "cursor-9") == 0);
    CHECK(snapshot.fresh);
    CHECK(snapshot.assistant_available);
    CHECK(snapshot.unread_count == 7U);
    CHECK(snapshot.active_task_count == 2U);
    CHECK(snapshot.message_count == 1U);
    CHECK(strcmp(snapshot.messages[0].preview, "Ready \xe4\xbd\xa0\xe5\xa5\xbd") == 0);
    CHECK(snapshot.messages[0].role == WB_MESSAGE_ROLE_ASSISTANT);
    CHECK(snapshot.messages[0].unread);
    CHECK(snapshot.task_count == 1U);
    CHECK(snapshot.tasks[0].status == WB_TASK_STATUS_RUNNING);
    CHECK(snapshot.output_count == 1U);
    CHECK(snapshot.outputs[0].kind == WB_OUTPUT_KIND_CHECKLIST);
    CHECK(strcmp(snapshot.outputs[0].task_id, "task-1") == 0);
    return 0;
}

static int test_rejects_unsupported_version(void)
{
    const char json[] =
        "{\"version\":2,\"request_id\":\"r\",\"cursor\":\"c\","
        "\"fresh\":true,\"assistant_available\":true,"
        "\"messages\":[],\"tasks\":[],\"outputs\":[]}";
    wb_snapshot_t snapshot;

    CHECK(wb_protocol_parse_snapshot(json, strlen(json), &snapshot) ==
          WB_PROTOCOL_ERR_UNSUPPORTED_VERSION);
    return 0;
}

static int test_rejects_malformed_or_trailing_json(void)
{
    wb_snapshot_t snapshot;
    const char truncated[] = "{\"version\":1";
    const char bad_escape[] = "{\"version\":1,\"x\":\"\\q\"}";
    const char trailing[] =
        "{\"version\":1,\"request_id\":\"r\",\"cursor\":\"c\","
        "\"fresh\":true,\"assistant_available\":true,"
        "\"messages\":[],\"tasks\":[],\"outputs\":[]} no";

    CHECK(wb_protocol_parse_snapshot(truncated, strlen(truncated), &snapshot) ==
          WB_PROTOCOL_ERR_INVALID_JSON);
    CHECK(wb_protocol_parse_snapshot(bad_escape, strlen(bad_escape), &snapshot) ==
          WB_PROTOCOL_ERR_INVALID_JSON);
    CHECK(wb_protocol_parse_snapshot(trailing, strlen(trailing), &snapshot) ==
          WB_PROTOCOL_ERR_INVALID_JSON);
    return 0;
}

static int test_reports_field_type_errors(void)
{
    wb_snapshot_t snapshot;
    const char wrong_boolean[] =
        "{\"version\":1,\"request_id\":\"r\",\"cursor\":\"c\","
        "\"fresh\":true,\"assistant_available\":\"yes\","
        "\"messages\":[],\"tasks\":[],\"outputs\":[]}";
    const char wrong_array[] =
        "{\"version\":1,\"request_id\":\"r\",\"cursor\":\"c\","
        "\"fresh\":true,\"assistant_available\":true,"
        "\"messages\":{},\"tasks\":[],\"outputs\":[]}";

    CHECK(wb_protocol_parse_snapshot(wrong_boolean, strlen(wrong_boolean), &snapshot) ==
          WB_PROTOCOL_ERR_TYPE);
    CHECK(wb_protocol_parse_snapshot(wrong_array, strlen(wrong_array), &snapshot) ==
          WB_PROTOCOL_ERR_TYPE);
    CHECK(strcmp(wb_protocol_result_name(WB_PROTOCOL_ERR_TYPE), "TYPE") == 0);
    return 0;
}

static int test_reports_missing_required_fields(void)
{
    const char json[] =
        "{\"version\":1,\"request_id\":\"r\","
        "\"fresh\":true,\"assistant_available\":true,"
        "\"messages\":[],\"tasks\":[],\"outputs\":[]}";
    wb_snapshot_t snapshot;

    CHECK(wb_protocol_parse_snapshot(json, strlen(json), &snapshot) ==
          WB_PROTOCOL_ERR_MISSING_FIELD);
    return 0;
}

static int test_truncates_display_strings_on_utf8_boundaries(void)
{
    char title[WB_TITLE_CAP + 8U];
    char preview[WB_PREVIEW_CAP + 8U];
    char json[2048];
    wb_snapshot_t snapshot;

    memset(title, 'a', WB_TITLE_MAX_BYTES - 1U);
    memcpy(&title[WB_TITLE_MAX_BYTES - 1U], "\xe4\xbd\xa0", 3U);
    title[WB_TITLE_MAX_BYTES + 2U] = '\0';
    memset(preview, 'p', sizeof(preview) - 1U);
    preview[sizeof(preview) - 1U] = '\0';

    (void)snprintf(
        json, sizeof(json),
        "{\"version\":1,\"request_id\":\"r\",\"cursor\":\"c\","
        "\"fresh\":true,\"assistant_available\":true,"
        "\"messages\":[{\"id\":\"m\",\"title\":\"%s\",\"preview\":\"%s\","
        "\"role\":\"assistant\",\"unread\":false}],"
        "\"tasks\":[],\"outputs\":[]}", title, preview);

    CHECK(wb_protocol_parse_snapshot(json, strlen(json), &snapshot) == WB_PROTOCOL_OK);
    CHECK(strlen(snapshot.messages[0].title) == WB_TITLE_MAX_BYTES - 1U);
    CHECK(strlen(snapshot.messages[0].preview) == WB_PREVIEW_MAX_BYTES);
    CHECK(snapshot.messages[0].title[WB_TITLE_CAP - 1U] == '\0');
    CHECK(snapshot.messages[0].preview[WB_PREVIEW_CAP - 1U] == '\0');
    return 0;
}

static int test_caps_each_collection_at_six_items(void)
{
    char json[4096];
    size_t used = 0U;
    unsigned index;
    wb_snapshot_t snapshot;

    used += (size_t)snprintf(
        &json[used], sizeof(json) - used,
        "{\"version\":1,\"request_id\":\"r\",\"cursor\":\"c\","
        "\"fresh\":true,\"assistant_available\":true,\"messages\":[");
    for (index = 0U; index < 8U; ++index) {
        used += (size_t)snprintf(
            &json[used], sizeof(json) - used,
            "%s{\"id\":\"m%u\",\"title\":\"t\",\"preview\":\"p\","
            "\"role\":\"assistant\",\"unread\":false}",
            index == 0U ? "" : ",", index);
    }
    (void)snprintf(&json[used], sizeof(json) - used, "],\"tasks\":[],\"outputs\":[]}");

    CHECK(wb_protocol_parse_snapshot(json, strlen(json), &snapshot) == WB_PROTOCOL_OK);
    CHECK(snapshot.message_count == WB_MAX_ITEMS);
    CHECK(strcmp(snapshot.messages[WB_MAX_ITEMS - 1U].id, "m5") == 0);
    return 0;
}

static int test_rejects_oversized_identifiers_and_bodies(void)
{
    char long_id[WB_ID_CAP + 1U];
    char json[1024];
    char *oversized;
    wb_snapshot_t snapshot;

    memset(long_id, 'x', sizeof(long_id) - 1U);
    long_id[sizeof(long_id) - 1U] = '\0';
    (void)snprintf(
        json, sizeof(json),
        "{\"version\":1,\"request_id\":\"%s\",\"cursor\":\"c\","
        "\"fresh\":true,\"assistant_available\":true,"
        "\"messages\":[],\"tasks\":[],\"outputs\":[]}", long_id);
    CHECK(wb_protocol_parse_snapshot(json, strlen(json), &snapshot) ==
          WB_PROTOCOL_ERR_LIMIT);

    oversized = malloc(WB_SNAPSHOT_MAX_BYTES + 1U);
    CHECK(oversized != NULL);
    memset(oversized, ' ', WB_SNAPSHOT_MAX_BYTES + 1U);
    CHECK(wb_protocol_parse_snapshot(oversized, WB_SNAPSHOT_MAX_BYTES + 1U, &snapshot) ==
          WB_PROTOCOL_ERR_LIMIT);
    free(oversized);
    return 0;
}

static int test_serializes_reply_action(void)
{
    char json[512];
    size_t length = 0U;

    CHECK(wb_protocol_serialize_reply("op-1", "message-1", "Hello", json,
                                      sizeof(json), &length) == WB_PROTOCOL_OK);
    CHECK(strcmp(json,
                 "{\"version\":1,\"operation_id\":\"op-1\",\"type\":\"reply\","
                 "\"message_id\":\"message-1\",\"text\":\"Hello\"}") == 0);
    CHECK(length == strlen(json));
    return 0;
}

static int test_serializes_task_create_with_json_escaping(void)
{
    char json[1024];
    const char text[] = "say \"hi\"\npath\\tab\t\x01";

    CHECK(wb_protocol_serialize_task_create("op-2", text, json, sizeof(json), NULL) ==
          WB_PROTOCOL_OK);
    CHECK(strcmp(json,
                 "{\"version\":1,\"operation_id\":\"op-2\","
                 "\"type\":\"task_create\","
                 "\"prompt\":\"say \\\"hi\\\"\\npath\\\\tab\\t\\u0001\"}") == 0);
    return 0;
}

static int test_serializes_task_followup_and_generic_action(void)
{
    char json[1024];
    wb_action_t action = { 0 };

    action.type = WB_ACTION_TASK_FOLLOWUP;
    snprintf(action.operation_id, sizeof(action.operation_id), "op-3");
    snprintf(action.target_id, sizeof(action.target_id), "task-7");
    snprintf(action.text, sizeof(action.text), "\xe7\xbb\xa7\xe7\xbb\xad");
    CHECK(wb_protocol_serialize_action(&action, json, sizeof(json), NULL) == WB_PROTOCOL_OK);
    CHECK(strcmp(json,
                 "{\"version\":1,\"operation_id\":\"op-3\","
                 "\"type\":\"task_followup\",\"task_id\":\"task-7\","
                 "\"text\":\"\xe7\xbb\xa7\xe7\xbb\xad\"}") == 0);
    CHECK(wb_protocol_serialize_task_followup("op-4", "task-8", "next", json,
                                               sizeof(json), NULL) == WB_PROTOCOL_OK);
    CHECK(strcmp(json,
                 "{\"version\":1,\"operation_id\":\"op-4\","
                 "\"type\":\"task_followup\",\"task_id\":\"task-8\","
                 "\"text\":\"next\"}") == 0);
    return 0;
}

static int test_serialization_reports_small_buffers_and_bad_arguments(void)
{
    char small[12];
    char json[1024];
    char long_text[WB_TRANSCRIPT_CAP + 1U];

    memset(small, 'x', sizeof(small));
    CHECK(wb_protocol_serialize_reply("op", "message", "hello", small,
                                      sizeof(small), NULL) ==
          WB_PROTOCOL_ERR_BUFFER_TOO_SMALL);
    CHECK(memchr(small, '\0', sizeof(small)) != NULL);
    CHECK(wb_protocol_serialize_reply("", "message", "hello", json,
                                      sizeof(json), NULL) ==
          WB_PROTOCOL_ERR_ARGUMENT);
    CHECK(wb_protocol_serialize_task_followup("op", "", "hello", json,
                                              sizeof(json), NULL) ==
          WB_PROTOCOL_ERR_ARGUMENT);

    memset(long_text, 'z', sizeof(long_text) - 1U);
    long_text[sizeof(long_text) - 1U] = '\0';
    CHECK(wb_protocol_serialize_task_create("op", long_text, json,
                                            sizeof(json), NULL) ==
          WB_PROTOCOL_ERR_LIMIT);
    return 0;
}

static int test_serialization_rejects_invalid_utf8(void)
{
    static const char invalid_lead[] = { (char)0x80, '\0' };
    static const char truncated[] = { (char)0xE2, (char)0x82, '\0' };
    static const char overlong[] = { (char)0xC0, (char)0xAF, '\0' };
    static const char surrogate[] = { (char)0xED, (char)0xA0, (char)0x80, '\0' };
    static const char too_high[] = {
        (char)0xF4, (char)0x90, (char)0x80, (char)0x80, '\0'
    };
    char json[1024];

    CHECK(wb_protocol_serialize_task_create("op", invalid_lead, json,
                                            sizeof(json), NULL) ==
          WB_PROTOCOL_ERR_INVALID_UTF8);
    CHECK(wb_protocol_serialize_task_create("op", truncated, json,
                                            sizeof(json), NULL) ==
          WB_PROTOCOL_ERR_INVALID_UTF8);
    CHECK(wb_protocol_serialize_reply("op", "message", overlong, json,
                                      sizeof(json), NULL) ==
          WB_PROTOCOL_ERR_INVALID_UTF8);
    CHECK(wb_protocol_serialize_task_followup("op", "task", surrogate, json,
                                              sizeof(json), NULL) ==
          WB_PROTOCOL_ERR_INVALID_UTF8);
    CHECK(wb_protocol_serialize_task_create("op", too_high, json,
                                            sizeof(json), NULL) ==
          WB_PROTOCOL_ERR_INVALID_UTF8);
    CHECK(wb_protocol_serialize_reply(invalid_lead, "message", "text", json,
                                      sizeof(json), NULL) ==
          WB_PROTOCOL_ERR_INVALID_UTF8);
    CHECK(wb_protocol_serialize_reply("op", invalid_lead, "text", json,
                                      sizeof(json), NULL) ==
          WB_PROTOCOL_ERR_INVALID_UTF8);
    CHECK(strcmp(wb_protocol_result_name(WB_PROTOCOL_ERR_INVALID_UTF8),
                 "INVALID_UTF8") == 0);
    return 0;
}

static int test_parser_reports_injected_allocation_failure(void)
{
    wb_protocol_result_t result;
    wb_snapshot_t snapshot;
    unsigned allocation_count;
    unsigned failure_index;

    s_allocation_calls = 0U;
    s_fail_allocation_at = UINT_MAX;
    wb_protocol_set_allocator(failing_allocate, test_deallocate);
    result = wb_protocol_parse_snapshot(s_valid_snapshot, strlen(s_valid_snapshot), &snapshot);
    wb_protocol_reset_allocator();
    CHECK(result == WB_PROTOCOL_OK);
    allocation_count = s_allocation_calls;
    CHECK(allocation_count > 1U);

    for (failure_index = 1U; failure_index <= allocation_count; ++failure_index) {
        s_allocation_calls = 0U;
        s_fail_allocation_at = failure_index;
        wb_protocol_set_allocator(failing_allocate, test_deallocate);
        result = wb_protocol_parse_snapshot(s_valid_snapshot, strlen(s_valid_snapshot),
                                            &snapshot);
        wb_protocol_reset_allocator();
        CHECK(result == WB_PROTOCOL_ERR_NO_MEMORY);
    }
    CHECK(strcmp(wb_protocol_result_name(WB_PROTOCOL_ERR_NO_MEMORY), "NO_MEMORY") == 0);
    return 0;
}

static int test_parser_enforces_depth_and_integer_boundaries(void)
{
    const char fractional_version[] =
        "{\"version\":1.5,\"request_id\":\"r\",\"cursor\":\"c\","
        "\"fresh\":true,\"assistant_available\":true,"
        "\"messages\":[],\"tasks\":[],\"outputs\":[]}";
    const char oversized_count[] =
        "{\"version\":1,\"request_id\":\"r\",\"cursor\":\"c\","
        "\"fresh\":true,\"assistant_available\":true,"
        "\"unread_count\":4294967296,"
        "\"messages\":[],\"tasks\":[],\"outputs\":[]}";
    char nested[1024];
    size_t used;
    unsigned index;
    wb_snapshot_t snapshot;

    CHECK(wb_protocol_parse_snapshot(fractional_version, strlen(fractional_version),
                                     &snapshot) == WB_PROTOCOL_ERR_TYPE);
    CHECK(wb_protocol_parse_snapshot(oversized_count, strlen(oversized_count),
                                     &snapshot) == WB_PROTOCOL_ERR_LIMIT);

    used = (size_t)snprintf(
        nested, sizeof(nested),
        "{\"version\":1,\"request_id\":\"r\",\"cursor\":\"c\","
        "\"fresh\":true,\"assistant_available\":true,\"extra\":");
    for (index = 0U; index < 15U; ++index) {
        nested[used++] = '[';
    }
    nested[used++] = '0';
    for (index = 0U; index < 15U; ++index) {
        nested[used++] = ']';
    }
    used += (size_t)snprintf(&nested[used], sizeof(nested) - used,
                             ",\"messages\":[],\"tasks\":[],\"outputs\":[]}");
    CHECK(wb_protocol_parse_snapshot(nested, used, &snapshot) == WB_PROTOCOL_OK);

    used = (size_t)snprintf(
        nested, sizeof(nested),
        "{\"version\":1,\"request_id\":\"r\",\"cursor\":\"c\","
        "\"fresh\":true,\"assistant_available\":true,\"extra\":");
    for (index = 0U; index < 16U; ++index) {
        nested[used++] = '[';
    }
    nested[used++] = '0';
    for (index = 0U; index < 16U; ++index) {
        nested[used++] = ']';
    }
    used += (size_t)snprintf(&nested[used], sizeof(nested) - used,
                             ",\"messages\":[],\"tasks\":[],\"outputs\":[]}");
    CHECK(wb_protocol_parse_snapshot(nested, used, &snapshot) == WB_PROTOCOL_ERR_LIMIT);
    return 0;
}

static int test_snapshot_parser_enforces_unicode_boundaries(void)
{
    const char invalid_raw[] =
        "{\"version\":1,\"request_id\":\"r\",\"cursor\":\"c\","
        "\"fresh\":true,\"assistant_available\":true,"
        "\"messages\":[{\"id\":\"m\",\"preview\":\""
        "\xc0\xaf"
        "\"}],\"tasks\":[],\"outputs\":[]}";
    const char lone_surrogate[] =
        "{\"version\":1,\"request_id\":\"r\",\"cursor\":\"c\","
        "\"fresh\":true,\"assistant_available\":true,"
        "\"messages\":[{\"id\":\"m\",\"preview\":\"\\ud800\"}],"
        "\"tasks\":[],\"outputs\":[]}";
    const char maximum_codepoint[] =
        "{\"version\":1,\"request_id\":\"r\",\"cursor\":\"c\","
        "\"fresh\":true,\"assistant_available\":true,"
        "\"messages\":[{\"id\":\"m\",\"preview\":\"\\udbff\\udfff\"}],"
        "\"tasks\":[],\"outputs\":[]}";
    wb_snapshot_t snapshot;

    CHECK(wb_protocol_parse_snapshot(invalid_raw, sizeof(invalid_raw) - 1U, &snapshot) ==
          WB_PROTOCOL_ERR_INVALID_UTF8);
    CHECK(wb_protocol_parse_snapshot(lone_surrogate, strlen(lone_surrogate), &snapshot) ==
          WB_PROTOCOL_ERR_INVALID_JSON);
    CHECK(wb_protocol_parse_snapshot(maximum_codepoint, strlen(maximum_codepoint),
                                     &snapshot) == WB_PROTOCOL_OK);
    CHECK(strcmp(snapshot.messages[0].preview, "\xf4\x8f\xbf\xbf") == 0);
    return 0;
}

int main(void)
{
    RUN_TEST(test_parses_version_one_snapshot);
    RUN_TEST(test_rejects_unsupported_version);
    RUN_TEST(test_rejects_malformed_or_trailing_json);
    RUN_TEST(test_reports_field_type_errors);
    RUN_TEST(test_reports_missing_required_fields);
    RUN_TEST(test_truncates_display_strings_on_utf8_boundaries);
    RUN_TEST(test_caps_each_collection_at_six_items);
    RUN_TEST(test_rejects_oversized_identifiers_and_bodies);
    RUN_TEST(test_serializes_reply_action);
    RUN_TEST(test_serializes_task_create_with_json_escaping);
    RUN_TEST(test_serializes_task_followup_and_generic_action);
    RUN_TEST(test_serialization_reports_small_buffers_and_bad_arguments);
    RUN_TEST(test_serialization_rejects_invalid_utf8);
    RUN_TEST(test_parser_reports_injected_allocation_failure);
    RUN_TEST(test_parser_enforces_depth_and_integer_boundaries);
    RUN_TEST(test_snapshot_parser_enforces_unicode_boundaries);
    printf("workbuddy protocol: %u tests passed\n", s_tests_run);
    return 0;
}
