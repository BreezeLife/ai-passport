/* Exercise the actual main.c orchestration with deterministic RTOS/I/O faults.
 * No duplicate application state machine and no physical device is accessed. */
#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <string.h>
#include "dino_stubs/dino_test_api.h"
#include "../main/main.c"

struct test_queue {
    unsigned length, item_size, head, count;
    uint8_t data[16][DINO_SAVE_SIZE];
};
static struct test_queue queues[8];
static unsigned queue_count, queue_calls, fail_queue_call;
static unsigned input_calls, stop_input_call, event_input_call;
static TickType_t input_waits[16];
static dino_input_t event_input;
static jmp_buf park;
static int64_t now_us;
static unsigned render_calls, lock_calls, fail_lock_call, lock_depth, refresh_calls;
static dino_model_t rendered;
static uint8_t backlight;
static const char *fail_task_name;
static unsigned storage_spawns, audio_spawns, scenario_count;
static bool button_script;
static bool motion_seed, motion_paused;
static bool motion_button;
static bsp_btn_t motion_button_key;
static unsigned motion_start_frame, motion_start_ms;
static uint8_t nvs_record[DINO_SAVE_SIZE];
static size_t nvs_record_size;
static esp_err_t button_error, nvs_init_error, nvs_read_error, nvs_write_error, nvs_commit_error;
static unsigned erase_calls, open_calls, read_calls, set_calls, commit_calls;
static unsigned audio_rate, audio_writes, audio_bytes, sleep_calls, wake_calls;
static esp_err_t audio_init_error, audio_wake_error, audio_write_error;
static int audio_hook;
static int16_t audio_first[8];
static uint8_t clip_a[320], clip_b[192];
static esp_partition_t audio_partition;
static bool partition_missing;
static uint8_t resource_header[DINO_AUDIO_BLOB_HEADER_BYTES];
static unsigned resource_reads, resource_payload_reads, fail_resource_read;
const uint8_t dino_audio_blob_header[DINO_AUDIO_BLOB_HEADER_BYTES] = {
    'D', 'I', 'N', 'O', 'A', '4', '0', 0,
    DINO_AUDIO_BLOB_SIZE & 0xff, (DINO_AUDIO_BLOB_SIZE >> 8) & 0xff,
    (DINO_AUDIO_BLOB_SIZE >> 16) & 0xff, (DINO_AUDIO_BLOB_SIZE >> 24) & 0xff,
};
const dino_audio_clip_t dino_audio_clips[DINO_AUDIO_CLIP_COUNT] = {
    [0] = { 64, sizeof(clip_a), sizeof(clip_a) * 2 },
    [1] = { 64 + sizeof(clip_a), sizeof(clip_b), sizeof(clip_b) * 2 },
    [3] = { 63, 1, 2 },
    [4] = { DINO_AUDIO_BLOB_SIZE, 1, 2 },
    [5] = { UINT32_MAX, 128, 256 },
    [6] = { 64, 1, 3 },
    [7] = { 64, 1, 0 },
    [8] = { 64, 2, 1 },
    [9] = { 64, 1, 1 },
};

static void reset(void) {
    memset(queues, 0, sizeof(queues));
    queue_count = queue_calls = fail_queue_call = input_calls = event_input_call = 0;
    stop_input_call = 5;
    memset(input_waits, 0, sizeof(input_waits));
    now_us = 0;
    render_calls = lock_calls = fail_lock_call = lock_depth = refresh_calls = 0;
    memset(&rendered, 0, sizeof(rendered));
    backlight = 0;
    fail_task_name = NULL;
    storage_spawns = audio_spawns = 0;
    button_script = false;
    motion_seed = motion_paused = false;
    motion_button = false;
    motion_button_key = BSP_BTN_UP;
    motion_start_frame = motion_start_ms = 0;
    nvs_record_size = 0;
    button_error = nvs_init_error = nvs_write_error = nvs_commit_error = ESP_OK;
    nvs_read_error = ESP_ERR_NVS_NOT_FOUND;
    erase_calls = open_calls = read_calls = set_calls = commit_calls = 0;
    audio_rate = audio_writes = audio_bytes = sleep_calls = wake_calls = 0;
    audio_init_error = audio_wake_error = audio_write_error = ESP_OK;
    audio_hook = 0;
    memset(audio_first, 0, sizeof(audio_first));
    memset(clip_a, 0x11, sizeof(clip_a));
    memset(clip_b, 0x22, sizeof(clip_b));
    audio_partition = (esp_partition_t){ .type = ESP_PARTITION_TYPE_DATA,
        .subtype = (esp_partition_subtype_t)0x40, .address = 0x35a000,
        .size = DINO_AUDIO_BLOB_SIZE, .erase_size = 4096, .label = "dino_audio" };
    partition_missing = false;
    resource_reads = resource_payload_reads = fail_resource_read = 0;
    memcpy(resource_header, dino_audio_blob_header, sizeof(resource_header));
    s_input = s_audio = s_save = NULL;
    atomic_store(&s_audio_status, 0);
    atomic_store(&s_storage_status, 0);
    atomic_store(&s_battery, -1);
    dino_model_init(&s_model);
}
static void run_main(void) {
    if (setjmp(park) == 0) app_main();
    assert(lock_depth == 0);
}
static void run_audio(void) {
    if (setjmp(park) == 0) audio_task(NULL);
}

QueueHandle_t xQueueCreate(unsigned length, unsigned item_size) {
    if (++queue_calls == fail_queue_call) return NULL;
    assert(queue_count < 8 && length <= 16 && item_size <= DINO_SAVE_SIZE);
    struct test_queue *q = &queues[queue_count++];
    q->length = length; q->item_size = item_size;
    return q;
}
BaseType_t xQueueSend(QueueHandle_t q, const void *item, TickType_t ticks) {
    (void)ticks;
    assert(q && item);
    if (q->count == q->length) return pdFALSE;
    memcpy(q->data[(q->head + q->count) % q->length], item, q->item_size);
    ++q->count;
    return pdTRUE;
}
BaseType_t xQueueOverwrite(QueueHandle_t q, const void *item) {
    assert(q && q->length == 1 && item);
    q->head = 0; q->count = 1;
    memcpy(q->data[0], item, q->item_size);
    return pdTRUE;
}
BaseType_t xQueueReceive(QueueHandle_t q, void *item, TickType_t ticks) {
    assert(q && item);
    if (q == s_input) {
        ++input_calls;
        if (input_calls <= sizeof(input_waits) / sizeof(input_waits[0]))
            input_waits[input_calls - 1] = ticks;
        now_us += (int64_t)ticks * 1000;
        if (input_calls == stop_input_call) longjmp(park, 1);
        if (input_calls == event_input_call) {
            memcpy(item, &event_input, sizeof(event_input));
            return pdTRUE;
        }
    }
    if (q->count) {
        memcpy(item, q->data[q->head], q->item_size);
        q->head = (q->head + 1) % q->length;
        --q->count;
        return pdTRUE;
    }
    if (ticks == portMAX_DELAY) longjmp(park, 1);
    return pdFALSE;
}
BaseType_t xTaskCreate(TaskFunction_t entry, const char *name, unsigned stack,
                      void *arg, unsigned priority, TaskHandle_t *task) {
    (void)entry; (void)stack; (void)arg; (void)priority;
    if (strcmp(name, "dino_save") == 0) ++storage_spawns;
    if (strcmp(name, "dino_audio") == 0) ++audio_spawns;
    if (task) *task = NULL;
    return fail_task_name && strcmp(name, fail_task_name) == 0 ? pdFALSE : pdPASS;
}
void vTaskDelete(TaskHandle_t task) { assert(task == NULL); }
void vTaskDelay(TickType_t ticks) { now_us += (int64_t)ticks * 1000; }
esp_err_t bsp_i2c_init(void) { return ESP_OK; }
esp_err_t bsp_display_init(void) { return ESP_OK; }
lv_display_t *bsp_lvgl_init(void) { static lv_display_t display; return &display; }
bool bsp_lvgl_lock(int timeout_ms) {
    if (timeout_ms == 250 && ++lock_calls == fail_lock_call) return false;
    assert(lock_depth == 0);
    ++lock_depth;
    return true;
}
void bsp_lvgl_unlock(void) { assert(lock_depth == 1); --lock_depth; }
void bsp_display_backlight(uint8_t percent) {
    if (percent > 0) assert(refresh_calls > 0);
    backlight = percent;
}
void lv_refr_now(lv_display_t *display) {
    (void)display;
    assert(lock_depth == 1 && render_calls > 0);
    ++refresh_calls;
}
void dino_ui_init(void) { assert(lock_depth == 1); }
void dino_ui_render(const dino_model_t *model, int battery, int audio, int storage) {
    (void)battery; (void)audio; (void)storage;
    assert(lock_depth == 1);
    rendered = *model;
    ++render_calls;
}
esp_err_t bsp_button_init(bsp_btn_cb_t callback, void *user) {
    if (motion_seed) {
        s_model.page = DINO_PAGE_MOTION;
        s_model.frame = (uint8_t)motion_start_frame;
        s_model.frame_ms = (uint16_t)motion_start_ms;
        s_model.paused = motion_paused;
    }
    if (motion_button) callback(motion_button_key, BSP_BTN_LONG, user);
    if (button_script && button_error == ESP_OK) {
        callback(BSP_BTN_OK, BSP_BTN_CLICK, user);
        callback(BSP_BTN_OK, BSP_BTN_LONG, user);
        callback(BSP_BTN_DOWN, BSP_BTN_CLICK, user);
        callback(BSP_BTN_OK, BSP_BTN_CLICK, user);
    }
    return button_error;
}
esp_err_t bsp_audio_init(void) { return audio_init_error; }
esp_err_t bsp_audio_set_format(uint32_t hz, uint8_t bits, uint8_t channels) {
    assert(bits == 16 && channels == 1); audio_rate = hz; return ESP_OK;
}
void bsp_audio_set_volume(uint8_t percent) { assert(percent <= 100); }
esp_err_t bsp_audio_write(const void *pcm, size_t bytes) {
    assert(bytes > 0 && bytes <= 512 && audio_writes < 8);
    audio_first[audio_writes++] = *(const int16_t *)pcm;
    audio_bytes += (unsigned)bytes;
    if (audio_writes == 1 && audio_hook) {
        int cancel = -1, next = 1;
        if (audio_hook == 1) (void)xQueueOverwrite(s_audio, &cancel);
        else if (audio_hook == 2) (void)xQueueOverwrite(s_audio, &next);
        else {
            (void)xQueueOverwrite(s_audio, &cancel);
            (void)xQueueOverwrite(s_audio, &next);
        }
    }
    return audio_write_error;
}
esp_err_t bsp_audio_sleep(void) { ++sleep_calls; return ESP_OK; }
esp_err_t bsp_audio_wake(void) { ++wake_calls; return audio_wake_error; }
esp_err_t bsp_battery_init(void) { return ESP_OK; }
int bsp_battery_soc(void) { return 70; }
int64_t esp_timer_get_time(void) { return now_us; }
size_t esp_get_free_heap_size(void) { return 65536; }
size_t heap_caps_get_largest_free_block(unsigned flags) { (void)flags; return 32768; }
esp_err_t nvs_flash_init(void) { return nvs_init_error; }
esp_err_t nvs_flash_erase(void) { ++erase_calls; return ESP_OK; }
esp_err_t nvs_open(const char *name, int mode, nvs_handle_t *handle) {
    (void)mode; assert(strcmp(name, "dinobook") == 0);
    ++open_calls; *handle = 1; return ESP_OK;
}
esp_err_t nvs_get_blob(nvs_handle_t handle, const char *key, void *bytes, size_t *size) {
    (void)handle; assert(strcmp(key, "progress") == 0);
    ++read_calls;
    if (nvs_record_size) {
        assert(*size >= nvs_record_size);
        memcpy(bytes, nvs_record, nvs_record_size); *size = nvs_record_size;
        return ESP_OK;
    }
    return nvs_read_error;
}
esp_err_t nvs_set_blob(nvs_handle_t handle, const char *key, const void *bytes, size_t size) {
    (void)handle; assert(strcmp(key, "progress") == 0 && bytes && size == DINO_SAVE_SIZE);
    ++set_calls; return nvs_write_error;
}
esp_err_t nvs_commit(nvs_handle_t handle) { (void)handle; ++commit_calls; return nvs_commit_error; }
void nvs_close(nvs_handle_t handle) { (void)handle; }
const char *esp_err_to_name(esp_err_t error) { (void)error; return "injected error"; }
void test_log(const char *tag, const char *format, ...) { (void)tag; (void)format; }
const esp_partition_t *esp_partition_find_first(esp_partition_type_t type,
                                               esp_partition_subtype_t subtype, const char *label) {
    assert(type == ESP_PARTITION_TYPE_DATA && (unsigned)subtype == 0x40);
    assert(strcmp(label, "dino_audio") == 0);
    return partition_missing ? NULL : &audio_partition;
}
esp_err_t esp_partition_read(const esp_partition_t *partition, size_t offset, void *out, size_t size) {
    assert(partition == &audio_partition && out && size > 0);
    assert(offset <= partition->size && size <= partition->size - offset);
    if (++resource_reads == fail_resource_read) return ESP_FAIL;
    if (offset == 0) {
        assert(size == DINO_AUDIO_BLOB_HEADER_BYTES);
        memcpy(out, resource_header, size);
    } else {
        ++resource_payload_reads;
        assert(size <= 128);
        if (offset >= 64 && offset + size <= 64 + sizeof(clip_a))
            memcpy(out, clip_a + offset - 64, size);
        else {
            assert(offset >= 64 + sizeof(clip_a) && offset + size <= 64 + sizeof(clip_a) + sizeof(clip_b));
            memcpy(out, clip_b + offset - 64 - sizeof(clip_a), size);
        }
    }
    return ESP_OK;
}

static void test_lock_retry(void) {
    ++scenario_count;
    reset();
    event_input_call = 2; event_input = DINO_INPUT_OK; fail_lock_call = 2;
    run_main();
    assert(lock_calls == 3 && render_calls == 3);
    assert(s_model.page == DINO_PAGE_CARD && rendered.page == DINO_PAGE_CARD);
    assert(s_model.dinosaur == 0 && rendered.dinosaur == 0);
    puts("main: one navigation survives a failed UI lock without a second input: PASS");
}
static void test_nvs_faults(void) {
    scenario_count += 4;
    reset(); nvs_init_error = ESP_FAIL; run_main();
    assert(erase_calls == 0 && open_calls == 0 && storage_spawns == 0);
    assert(atomic_load(&s_storage_status) == -1 && render_calls > 0);
    reset(); nvs_read_error = ESP_FAIL; run_main();
    assert(read_calls == 1 && erase_calls == 0 && storage_spawns == 0);
    assert(atomic_load(&s_storage_status) == -1 && render_calls > 0);
    for (unsigned commit_failure = 0; commit_failure < 2; ++commit_failure) {
        reset(); s_save = xQueueCreate(1, DINO_SAVE_SIZE);
        uint8_t record[DINO_SAVE_SIZE]; dino_model_save(&s_model, record);
        (void)xQueueOverwrite(s_save, record);
        if (commit_failure) nvs_commit_error = ESP_FAIL;
        else nvs_write_error = ESP_FAIL;
        if (setjmp(park) == 0) storage_task(NULL);
        assert(set_calls == 1 && commit_calls == commit_failure && erase_calls == 0);
        assert(atomic_load(&s_storage_status) == -1);
    }
    puts("main: NVS init/read/write faults degrade without erase or destructive startup writes: PASS");
}
static void test_audio_requests(void) {
    for (int mode = 1; mode <= 3; ++mode) {
        ++scenario_count;
        reset(); s_audio = xQueueCreate(1, sizeof(int)); audio_hook = mode;
        int first = 0; (void)xQueueOverwrite(s_audio, &first); run_audio();
        assert(audio_rate == DINO_AUDIO_SAMPLE_RATE && audio_rate == 8000);
        assert(sleep_calls > 0);
        if (mode == 1) assert(audio_writes == 1 && audio_bytes == 512);
        else {
            assert(audio_writes == 3 && audio_bytes == 1280);
            assert(audio_first[0] == 1 && audio_first[1] == 3);
        }
    }
    puts("main: 8 kHz, cancellation and replacement consume the latest request without resuming an old clip: PASS");
}
static void test_idle_audio(void) {
    ++scenario_count;
    reset(); s_audio = xQueueCreate(1, sizeof(int)); run_audio();
    assert(audio_writes == 0 && sleep_calls > 0);
    puts("main: no queued narration leaves the codec asleep at muted startup: PASS");
}
static void test_mute_input(void) {
    ++scenario_count;
    reset(); button_script = true; run_main();
    assert(s_model.page == DINO_PAGE_CAMP && s_model.muted);
    assert(s_audio->count == 1);
    int request; memcpy(&request, s_audio->data[s_audio->head], sizeof(request));
    assert(request == -1);
    run_audio();
    assert(audio_writes == 0 && s_audio->count == 0 && sleep_calls > 0);
    puts("main: the actual camp mute input replaces queued welcome/name narration with cancellation: PASS");
}
static void test_failed_startup(void) {
    const char *names[] = { "dino_audio", "dino_save", "dino_battery" };
    for (unsigned i = 0; i < 3; ++i) {
        ++scenario_count;
        reset(); fail_task_name = names[i]; stop_input_call = 903; run_main();
        assert(backlight == 0);
    }
    ++scenario_count;
    reset(); button_error = ESP_FAIL; run_main();
    assert(backlight == 0);
    puts("main: worker creation/button failures do not leave the backlight permanently on: PASS");
}
static void test_queue_allocation(void) {
    ++scenario_count;
    reset(); fail_queue_call = 1; run_main();
    assert(s_input == NULL && backlight == 0 && render_calls == 0);
    assert(audio_spawns == 0 && storage_spawns == 0);
    for (unsigned failed = 2; failed <= 3; ++failed) {
        ++scenario_count;
        reset(); fail_queue_call = failed; event_input_call = 2;
        event_input = DINO_INPUT_OK; stop_input_call = 903; run_main();
        assert(s_model.page == DINO_PAGE_CARD && rendered.page == DINO_PAGE_CARD);
        assert(render_calls > 0 && backlight == 0);
        if (failed == 2) {
            assert(s_audio == NULL && audio_spawns == 0);
            assert(atomic_load(&s_audio_status) == -1);
        } else {
            assert(s_save == NULL && storage_spawns == 0);
            assert(atomic_load(&s_storage_status) == -1);
        }
    }
    puts("main: optional queue allocation failures preserve navigation and idle-off; input failure is safe: PASS");
}
static void test_codec_faults(void) {
    ++scenario_count;
    reset(); s_audio = xQueueCreate(1, sizeof(int)); audio_init_error = ESP_FAIL;
    run_audio();
    assert(audio_writes == 0 && atomic_load(&s_audio_status) == -1);
    for (unsigned write_failure = 0; write_failure < 2; ++write_failure) {
        ++scenario_count;
        reset(); s_audio = xQueueCreate(1, sizeof(int));
        int request = 0; (void)xQueueOverwrite(s_audio, &request);
        if (write_failure) audio_write_error = ESP_FAIL;
        else audio_wake_error = ESP_FAIL;
        run_audio();
        assert(audio_writes == write_failure && atomic_load(&s_audio_status) == -1);
        assert(s_audio->count == 0 && sleep_calls > 0);
    }
    puts("main: codec init/wake/write failures stop narration and report degradation: PASS");
}
static void test_motion_timing(void) {
    ++scenario_count;
    reset(); motion_seed = true; run_main();
    assert(input_waits[0] == 100 && input_waits[1] == 25 &&
           input_waits[2] == 100 && input_waits[3] == 25);
    assert(s_model.frame == 2 && s_model.frame_ms == 0);
    assert(rendered.frame == 2 && rendered.frame_ms == 0);
    assert(s_save->count == 0);
    ++scenario_count;
    reset(); motion_seed = true; event_input_call = 2; event_input = DINO_INPUT_OK;
    run_main();
    assert(s_model.paused && s_model.frame == 1 && s_model.frame_ms == 0);
    assert(input_waits[2] == 100 && input_waits[3] == 100);
    ++scenario_count;
    reset(); motion_seed = motion_paused = true; motion_start_frame = 3; motion_start_ms = 20;
    event_input_call = 3; event_input = DINO_INPUT_OK; run_main();
    assert(!s_model.paused && s_model.frame == 3 && s_model.frame_ms == 120);
    ++scenario_count;
    reset(); motion_seed = true; event_input_call = 2; event_input = DINO_INPUT_DOWN;
    run_main();
    assert(s_model.dinosaur == 1 && s_model.frame == 1 && s_model.frame_ms == 0);
    ++scenario_count;
    reset(); motion_seed = true; stop_input_call = 1455; run_main();
    assert(backlight == 0 && s_model.frame == 0 && s_model.frame_ms == 0);
    ++scenario_count;
    reset(); motion_seed = true; stop_input_call = 1454;
    event_input_call = 1452; event_input = DINO_INPUT_OK; run_main();
    assert(backlight == 80 && !s_model.paused && s_model.frame == 0 && s_model.frame_ms == 100);
    puts("main: motion ticks precede inputs, exclude paused/dark time, preserve switch/replay timing and do not save frames: PASS");
}
static void test_long_buttons(void) {
    ++scenario_count;
    reset(); s_input = xQueueCreate(16, sizeof(dino_input_t));
    on_key(BSP_BTN_UP, BSP_BTN_LONG, NULL);
    dino_input_t input;
    assert(xQueueReceive(s_input, &input, 0) == pdTRUE && input == DINO_INPUT_EXIT);
    ++scenario_count;
    on_key(BSP_BTN_OK, BSP_BTN_LONG, NULL);
    assert(xQueueReceive(s_input, &input, 0) == pdTRUE && input == DINO_INPUT_BACK);
    puts("main: long UP exits motion; long OK remains replay/back: PASS");
}
static void test_motion_button_flow(void) {
    ++scenario_count;
    reset(); motion_seed = motion_paused = motion_button = true;
    motion_start_frame = 6; motion_start_ms = 43;
    run_main();
    assert(s_model.page == DINO_PAGE_CAMP && s_model.camp == 2);
    assert(rendered.page == DINO_PAGE_CAMP && s_model.frame == 6 && s_model.frame_ms == 43);
    ++scenario_count;
    reset(); motion_seed = motion_paused = motion_button = true;
    motion_start_frame = 6; motion_start_ms = 43; motion_button_key = BSP_BTN_OK;
    run_main();
    assert(s_model.page == DINO_PAGE_MOTION && !s_model.paused);
    assert(s_model.frame == 1 && s_model.frame_ms == 100);
    puts("main: actual long-button callbacks exit to camp or replay from zero and resume: PASS");
}
static void test_welcome(void) {
    ++scenario_count;
    reset(); run_main();
    int request; assert(s_audio->count == 1);
    memcpy(&request, s_audio->data[s_audio->head], sizeof(request));
    assert(request == DINO_AUDIO_WELCOME);
    puts("main: welcome uses the 40-species catalogue ID: PASS");
}
static void test_legacy_nvs(void) {
    ++scenario_count;
    reset();
    const uint8_t legacy[DINO_SAVE_V1_SIZE] = {
        0x44, 0x49, 0x4e, 0x4f, 1, 0xa5, 1, 7, 0, 0, 0, 0,
        0xad, 0x65, 0xa8, 0xc4,
    };
    memcpy(nvs_record, legacy, sizeof(legacy)); nvs_record_size = sizeof(legacy);
    run_main();
    assert(s_model.found == 0xa5 && s_model.dinosaur == 7 && s_model.muted);
    assert(s_model.page == DINO_PAGE_INTRO && set_calls == 0 && commit_calls == 0 && erase_calls == 0);
    puts("main: actual NVS startup accepts the legacy 16-byte record without immediate rewriting: PASS");
}
static void test_resource_startup(void) {
    for (unsigned mode = 0; mode < 4; ++mode) {
        ++scenario_count;
        reset(); s_audio = xQueueCreate(1, sizeof(int));
        int request = 0; (void)xQueueOverwrite(s_audio, &request);
        if (mode == 0) partition_missing = true;
        else if (mode == 1) resource_header[12] ^= 1;
        else if (mode == 2) fail_resource_read = 1;
        else audio_partition.size = DINO_AUDIO_BLOB_SIZE - 1;
        run_audio();
        assert(audio_writes == 0 && wake_calls == 0 && atomic_load(&s_audio_status) == -1);
        assert(resource_payload_reads == 0);
    }
    puts("main: missing, short, mismatched and unreadable audio banks degrade without starting PCM: PASS");
}
static void test_resource_read_failure(void) {
    ++scenario_count;
    reset(); s_audio = xQueueCreate(1, sizeof(int));
    int request = 0; (void)xQueueOverwrite(s_audio, &request);
    fail_resource_read = 3;
    run_audio();
    assert(resource_reads == 3 && audio_writes == 1 && audio_bytes == 512);
    assert(atomic_load(&s_audio_status) == -1 && sleep_calls >= 2);
    puts("main: a mid-clip partition read failure stops further PCM and sleeps the codec: PASS");
}
static void test_audio_descriptors(void) {
    const int invalid[] = { 2, 3, 4, 5, 6, 7, 8, DINO_AUDIO_CLIP_COUNT };
    for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        ++scenario_count;
        reset(); s_audio = xQueueCreate(1, sizeof(int));
        (void)xQueueOverwrite(s_audio, &invalid[i]); run_audio();
        assert(resource_reads == 1 && audio_writes == 0 && wake_calls == 0);
        assert(atomic_load(&s_audio_status) == -1);
    }
    ++scenario_count;
    reset(); s_audio = xQueueCreate(1, sizeof(int));
    int odd = 9; (void)xQueueOverwrite(s_audio, &odd); run_audio();
    assert(audio_writes == 1 && audio_bytes == 2 && atomic_load(&s_audio_status) == 1);
    puts("main: invalid clip bounds/sample counts never read or wake; odd final padding is not played: PASS");
}
int main(int argc, char **argv) {
    if (argc > 1) {
        if (strcmp(argv[1], "timing") == 0) test_motion_timing();
        else if (strcmp(argv[1], "buttons") == 0) test_long_buttons();
        else if (strcmp(argv[1], "welcome") == 0) test_welcome();
        else if (strcmp(argv[1], "resources") == 0) test_resource_startup();
        else if (strcmp(argv[1], "readfault") == 0) test_resource_read_failure();
        else if (strcmp(argv[1], "descriptors") == 0) test_audio_descriptors();
        else return 2;
        return 0;
    }
    test_lock_retry();
    test_nvs_faults();
    test_audio_requests();
    test_idle_audio();
    test_mute_input();
    test_failed_startup();
    test_queue_allocation();
    test_codec_faults();
    test_motion_timing();
    test_long_buttons();
    test_motion_button_flow();
    test_welcome();
    test_legacy_nvs();
    test_resource_startup();
    test_resource_read_failure();
    test_audio_descriptors();
    printf("Runtime fault scenarios: %u\n", scenario_count);
    puts("actual DinoBook runtime fault-injection tests: PASS");
    return 0;
}
