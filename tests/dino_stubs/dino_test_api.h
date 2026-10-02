#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>

typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL (-1)
#define ESP_ERR_NO_MEM 0x101
#define ESP_ERR_NVS_NOT_FOUND 0x1102
#define ESP_ERR_NVS_INVALID_LENGTH 0x110c
#define MALLOC_CAP_INTERNAL 1
typedef int BaseType_t;
typedef uint32_t TickType_t;
#define pdTRUE 1
#define pdFALSE 0
#define pdPASS 1
#define portMAX_DELAY UINT32_MAX
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))
typedef void (*TaskFunction_t)(void *);
typedef void *TaskHandle_t;
struct test_queue;
typedef struct test_queue *QueueHandle_t;
typedef enum { BSP_BTN_UP, BSP_BTN_DOWN, BSP_BTN_OK } bsp_btn_t;
typedef enum { BSP_BTN_PRESS, BSP_BTN_CLICK, BSP_BTN_DOUBLE, BSP_BTN_LONG } bsp_btn_ev_t;
typedef void (*bsp_btn_cb_t)(bsp_btn_t, bsp_btn_ev_t, void *);
typedef struct { unsigned marker; } lv_display_t;
typedef unsigned nvs_handle_t;
typedef struct esp_flash_t esp_flash_t;
typedef enum { ESP_PARTITION_TYPE_APP = 0, ESP_PARTITION_TYPE_DATA = 1 } esp_partition_type_t;
typedef enum { ESP_PARTITION_SUBTYPE_ANY = 0xff } esp_partition_subtype_t;
typedef struct {
    esp_flash_t *flash_chip;
    esp_partition_type_t type;
    esp_partition_subtype_t subtype;
    uint32_t address, size, erase_size;
    char label[17];
    bool encrypted, readonly;
} esp_partition_t;
#define NVS_READONLY 0
#define NVS_READWRITE 1

QueueHandle_t xQueueCreate(unsigned length, unsigned item_size);
BaseType_t xQueueSend(QueueHandle_t queue, const void *item, TickType_t ticks);
BaseType_t xQueueOverwrite(QueueHandle_t queue, const void *item);
BaseType_t xQueueReceive(QueueHandle_t queue, void *item, TickType_t ticks);
BaseType_t xTaskCreate(TaskFunction_t entry, const char *name, unsigned stack,
                      void *arg, unsigned priority, TaskHandle_t *task);
void vTaskDelete(TaskHandle_t task);
void vTaskDelay(TickType_t ticks);
esp_err_t bsp_i2c_init(void);
esp_err_t bsp_display_init(void);
lv_display_t *bsp_lvgl_init(void);
bool bsp_lvgl_lock(int timeout_ms);
void bsp_lvgl_unlock(void);
void bsp_display_backlight(uint8_t percent);
void lv_refr_now(lv_display_t *display);
esp_err_t bsp_button_init(bsp_btn_cb_t callback, void *user);
esp_err_t bsp_audio_init(void);
esp_err_t bsp_audio_set_format(uint32_t hz, uint8_t bits, uint8_t channels);
void bsp_audio_set_volume(uint8_t percent);
esp_err_t bsp_audio_write(const void *pcm, size_t bytes);
esp_err_t bsp_audio_sleep(void);
esp_err_t bsp_audio_wake(void);
esp_err_t bsp_battery_init(void);
int bsp_battery_soc(void);
int64_t esp_timer_get_time(void);
size_t esp_get_free_heap_size(void);
size_t heap_caps_get_largest_free_block(unsigned flags);
esp_err_t nvs_flash_init(void);
esp_err_t nvs_flash_erase(void);
esp_err_t nvs_open(const char *name, int mode, nvs_handle_t *handle);
esp_err_t nvs_get_blob(nvs_handle_t handle, const char *key, void *bytes, size_t *size);
esp_err_t nvs_set_blob(nvs_handle_t handle, const char *key, const void *bytes, size_t size);
esp_err_t nvs_commit(nvs_handle_t handle);
void nvs_close(nvs_handle_t handle);
const char *esp_err_to_name(esp_err_t error);
const esp_partition_t *esp_partition_find_first(esp_partition_type_t type,
                                               esp_partition_subtype_t subtype, const char *label);
esp_err_t esp_partition_read(const esp_partition_t *partition, size_t offset, void *out, size_t size);
void test_log(const char *tag, const char *format, ...);
#define ESP_LOGI(...) test_log(__VA_ARGS__)
#define ESP_LOGW(...) test_log(__VA_ARGS__)
#define ESP_LOGE(...) test_log(__VA_ARGS__)
