#include "bsp_i2c.h"
#include "bsp_display.h"
#include "bsp_button.h"
#include "bsp_audio.h"
#include "bsp_battery.h"
#include "dino_model.h"
#include "dino_catalog.h"
#include "dino_ui.h"
#include "dino_adpcm.h"
#include "dino_audio_assets.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_partition.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "lvgl.h"
#include <stdatomic.h>
#include <string.h>

static const char *TAG = "dinobook";
static QueueHandle_t s_input, s_audio, s_save;
static atomic_int s_audio_status, s_storage_status, s_battery;
static dino_model_t s_model;

/* Shared esp_timer callback: enqueue only; no UI, I/O or waiting. */
static void on_key(bsp_btn_t button, bsp_btn_ev_t event, void *user) {
    (void)user;
    dino_input_t input;
    if (button == BSP_BTN_UP && event == BSP_BTN_LONG) input = DINO_INPUT_EXIT;
    else if (button == BSP_BTN_OK && event == BSP_BTN_LONG) input = DINO_INPUT_BACK;
    else if (event == BSP_BTN_CLICK || event == BSP_BTN_DOUBLE) {
        input = button == BSP_BTN_UP ? DINO_INPUT_UP :
                button == BSP_BTN_DOWN ? DINO_INPUT_DOWN : DINO_INPUT_OK;
    } else return;
    if (s_input) (void)xQueueSend(s_input, &input, 0);
}

/* This worker alone owns codec I/O. A latest request interrupts between
 * 256-sample PCM chunks; formats never change mid-stream. */
static void audio_task(void *arg) {
    (void)arg;
    const esp_partition_t *bank = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, (esp_partition_subtype_t)0x40, "dino_audio");
    uint8_t header[DINO_AUDIO_BLOB_HEADER_BYTES];
    if (!bank || DINO_AUDIO_BLOB_SIZE < sizeof(header) ||
        bank->size < DINO_AUDIO_BLOB_SIZE ||
        esp_partition_read(bank, 0, header, sizeof(header)) != ESP_OK ||
        memcmp(header, dino_audio_blob_header, sizeof(header)) != 0) {
        atomic_store(&s_audio_status, -1);
        ESP_LOGW(TAG, "Narration bank missing, unreadable or from another build");
        vTaskDelete(NULL);
        return;
    }
    if (bsp_audio_init() != ESP_OK ||
        bsp_audio_set_format(DINO_AUDIO_SAMPLE_RATE, 16, 1) != ESP_OK) {
        atomic_store(&s_audio_status, -1);
        vTaskDelete(NULL);
        return;
    }
    bsp_audio_set_volume(35);
    atomic_store(&s_audio_status, 1);
    /* A muted boot has no initial request, so start the codec asleep too. */
    if (bsp_audio_sleep() != ESP_OK) atomic_store(&s_audio_status, -1);
    bool awake = false;
    int requested;
    int16_t pcm[256];
    uint8_t compressed[128];
    for (;;) {
        if (xQueueReceive(s_audio, &requested, portMAX_DELAY) != pdTRUE) continue;
        while (requested >= 0) {
            if (requested >= DINO_AUDIO_CLIP_COUNT) {
                atomic_store(&s_audio_status, -1);
                break;
            }
            const dino_audio_clip_t *clip = &dino_audio_clips[requested];
            /* Bounds are subtraction-based so a corrupt uint32 offset cannot
             * wrap through the bank end. Require exactly one optional pad nibble. */
            if (clip->offset < sizeof(header) || clip->offset > DINO_AUDIO_BLOB_SIZE ||
                !clip->bytes || clip->bytes > DINO_AUDIO_BLOB_SIZE - clip->offset ||
                !clip->samples || clip->samples / 2 + clip->samples % 2 != clip->bytes) {
                atomic_store(&s_audio_status, -1);
                ESP_LOGW(TAG, "Invalid narration clip descriptor");
                break;
            }
            if (!awake) {
                if (bsp_audio_wake() != ESP_OK) {
                    atomic_store(&s_audio_status, -1);
                    break;
                }
                awake = true;
                atomic_store(&s_audio_status, 1);
            }
            dino_adpcm_state_t decoder;
            (void)dino_adpcm_init(&decoder, 0, 0);
            bool interrupted = false;
            size_t samples = 0;
            for (size_t offset = 0; offset < clip->bytes;) {
                int next;
                if (xQueueReceive(s_audio, &next, 0) == pdTRUE) {
                    requested = next;
                    interrupted = true;
                    break;
                }
                size_t bytes = clip->bytes - offset;
                if (bytes > sizeof(compressed)) bytes = sizeof(compressed);
                if (esp_partition_read(bank, clip->offset + offset, compressed, bytes) != ESP_OK) {
                    atomic_store(&s_audio_status, -1);
                    ESP_LOGW(TAG, "Narration bank read failed; clip stopped");
                    break;
                }
                size_t decoded = dino_adpcm_decode(&decoder, compressed, bytes, pcm, 256);
                if (!decoded) break;
                if (decoded > clip->samples - samples) decoded = clip->samples - samples;
                if (bsp_audio_write(pcm, decoded * sizeof(pcm[0])) != ESP_OK) {
                    atomic_store(&s_audio_status, -1);
                    break;
                }
                samples += decoded;
                offset += bytes;
            }
            if (!interrupted) break;
        }
        /* No PCM I/O remains in flight in this task. Stop I2S and suspend the
         * codec at the end of playback/cancellation instead of idling awake. */
        if (awake) {
            if (bsp_audio_sleep() != ESP_OK) atomic_store(&s_audio_status, -1);
            awake = false;
        }
    }
}

/* Coalesce browsing writes. Only this task writes NVS. Power loss within
 * the 700 ms debounce window can lose the most recent action. */
static void storage_task(void *arg) {
    (void)arg;
    nvs_handle_t handle;
    if (nvs_open("dinobook", NVS_READWRITE, &handle) != ESP_OK) {
        atomic_store(&s_storage_status, -1);
        vTaskDelete(NULL);
        return;
    }
    uint8_t snapshot[DINO_SAVE_SIZE], last[DINO_SAVE_SIZE];
    memset(last, 0, sizeof(last));
    for (;;) {
        if (xQueueReceive(s_save, snapshot, portMAX_DELAY) != pdTRUE) continue;
        vTaskDelay(pdMS_TO_TICKS(700));
        uint8_t newer[DINO_SAVE_SIZE];
        while (xQueueReceive(s_save, newer, 0) == pdTRUE) memcpy(snapshot, newer, sizeof(snapshot));
        if (memcmp(snapshot, last, sizeof(snapshot)) == 0) continue;
        esp_err_t error = nvs_set_blob(handle, "progress", snapshot, sizeof(snapshot));
        if (error == ESP_OK) error = nvs_commit(handle);
        atomic_store(&s_storage_status, error == ESP_OK ? 1 : -1);
        if (error == ESP_OK) memcpy(last, snapshot, sizeof(last));
        else ESP_LOGW(TAG, "Progress save failed: %s", esp_err_to_name(error));
    }
}
static void battery_task(void *arg) {
    (void)arg;
    bool ready = bsp_battery_init() == ESP_OK;
    for (;;) {
        atomic_store(&s_battery, ready ? bsp_battery_soc() : -1);
        vTaskDelay(pdMS_TO_TICKS(30000));
    }
}
static void load_progress(void) {
    dino_model_init(&s_model);
    esp_err_t error = nvs_flash_init();
    if (error != ESP_OK) {
        /* Never erase shared settings as a recovery shortcut. */
        atomic_store(&s_storage_status, -1);
        ESP_LOGW(TAG, "NVS unavailable: %s", esp_err_to_name(error));
        return;
    }
    atomic_store(&s_storage_status, 1);
    nvs_handle_t handle;
    error = nvs_open("dinobook", NVS_READONLY, &handle);
    if (error == ESP_ERR_NVS_NOT_FOUND) return;
    if (error != ESP_OK) {
        atomic_store(&s_storage_status, -1);
        ESP_LOGW(TAG, "Progress namespace read failed: %s", esp_err_to_name(error));
        return;
    }
    uint8_t bytes[DINO_SAVE_SIZE];
    size_t size = sizeof(bytes);
    error = nvs_get_blob(handle, "progress", bytes, &size);
    if (error == ESP_OK && !dino_model_load(&s_model, bytes, size))
        ESP_LOGW(TAG, "Invalid progress record; using defaults without erasing NVS");
    else if (error != ESP_OK && error != ESP_ERR_NVS_NOT_FOUND &&
             error != ESP_ERR_NVS_INVALID_LENGTH) {
        /* Unknown read failure must not turn default state into a destructive
         * replacement for an existing record. Disable the writer this boot. */
        atomic_store(&s_storage_status, -1);
        ESP_LOGW(TAG, "Progress read failed: %s", esp_err_to_name(error));
    }
    nvs_close(handle);
}
void app_main(void) {
    ESP_LOGI(TAG, "Dino Passport starting");
    atomic_store(&s_battery, -1);
    load_progress();
    if (bsp_i2c_init() != ESP_OK) ESP_LOGW(TAG, "Optional I2C unavailable");
    if (bsp_display_init() != ESP_OK || !bsp_lvgl_init()) {
        ESP_LOGE(TAG, "Display initialization failed");
        return;
    }
    bsp_display_backlight(0);
    s_input = xQueueCreate(16, sizeof(dino_input_t));
    s_audio = xQueueCreate(1, sizeof(int));
    s_save = xQueueCreate(1, DINO_SAVE_SIZE);
    if (!s_input) {
        ESP_LOGE(TAG, "Input queue allocation failed");
        return;
    }
    if (!s_audio) atomic_store(&s_audio_status, -1);
    if (!s_save) atomic_store(&s_storage_status, -1);
    if (!bsp_lvgl_lock(1000)) return;
    dino_ui_init();
    dino_ui_render(&s_model, -1, atomic_load(&s_audio_status), atomic_load(&s_storage_status));
    lv_refr_now(NULL);
    bsp_lvgl_unlock();
    bsp_display_backlight(80);
    if (s_audio && xTaskCreate(audio_task, "dino_audio", 4096, NULL, 3, NULL) != pdPASS)
        atomic_store(&s_audio_status, -1);
    if (s_save && atomic_load(&s_storage_status) > 0 &&
        xTaskCreate(storage_task, "dino_save", 3072, NULL, 2, NULL) != pdPASS)
        atomic_store(&s_storage_status, -1);
    if (xTaskCreate(battery_task, "dino_battery", 3072, NULL, 1, NULL) != pdPASS)
        atomic_store(&s_battery, -1);
    if (bsp_button_init(on_key, NULL) != ESP_OK) {
        ESP_LOGE(TAG, "Button initialization failed; restart after checking hardware");
        bsp_display_backlight(0);
        return;
    }
    if (s_audio && !s_model.muted) {
        int welcome = DINO_AUDIO_WELCOME;
        (void)xQueueOverwrite(s_audio, &welcome);
    }
    ESP_LOGI(TAG, "ready; free=%u largest=%u", (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    int64_t last_input = esp_timer_get_time();
    int64_t last_tick = last_input;
    int last_battery = -2, last_audio = -2, last_storage = -2;
    unsigned backlight = 80;
    bool render_pending = true;
    for (;;) {
        dino_input_t input;
        unsigned wait_ms = 100;
        if (backlight != 0 && s_model.page == DINO_PAGE_MOTION && !s_model.paused) {
            unsigned until_frame = DINO_FRAME_MS - s_model.frame_ms;
            if (until_frame < wait_ms) wait_ms = until_frame;
        }
        if (wait_ms == 0) wait_ms = 1;
        TickType_t wait_ticks = pdMS_TO_TICKS(wait_ms);
        if (wait_ticks == 0) wait_ticks = 1;
        bool received = xQueueReceive(s_input, &input, wait_ticks) == pdTRUE;
        int64_t now = esp_timer_get_time();
        uint32_t elapsed_ms = (uint32_t)((now - last_tick) / 1000);
        last_tick = now;
        /* Charge elapsed time to the old playback state before an input can
         * pause, resume, restart or select another dinosaur. Dark time is free. */
        bool render = backlight != 0 && dino_model_tick(&s_model, elapsed_ms);
        if (received) {
            last_input = now;
            bool waking = backlight == 0;
            backlight = 80;
            bsp_display_backlight(80);
            if (!waking) {
                dino_effect_t effect = dino_model_handle(&s_model, input,
                    dino_catalog[s_model.dinosaur].correct);
                render = render || effect.changed;
                if (s_audio && (effect.changed || effect.audio >= 0)) {
                    int clip = s_model.muted ? -1 : effect.audio;
                    (void)xQueueOverwrite(s_audio, &clip);
                }
                if (s_save && effect.persist) {
                    uint8_t bytes[DINO_SAVE_SIZE];
                    dino_model_save(&s_model, bytes);
                    (void)xQueueOverwrite(s_save, bytes);
                }
            }
        }
        int battery = atomic_load(&s_battery);
        int audio = atomic_load(&s_audio_status);
        int storage = atomic_load(&s_storage_status);
        if (battery != last_battery || audio != last_audio || storage != last_storage) render = true;
        render_pending = render_pending || render;
        if (render_pending && bsp_lvgl_lock(250)) {
            dino_ui_render(&s_model, battery, audio, storage);
            bsp_lvgl_unlock();
            last_battery = battery;
            last_audio = audio;
            last_storage = storage;
            render_pending = false;
        }
        int64_t idle = esp_timer_get_time() - last_input;
        unsigned target = idle >= 90000000 ? 0 : idle >= 45000000 ? 20 : 80;
        if (target != backlight) {
            backlight = target;
            bsp_display_backlight(target);
        }
    }
}
