#pragma once

#include "esp_err.h"

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    WB_WIFI_DISABLED = 0,
    WB_WIFI_STARTING,
    WB_WIFI_CONNECTING,
    WB_WIFI_SYNCING_TIME,
    WB_WIFI_CONNECTED,
    WB_WIFI_BACKOFF,
    WB_WIFI_FAILED,
} wb_wifi_state_t;

/* Starts one STA interface. In demo mode this is a successful no-op. */
esp_err_t wb_wifi_start(void);

bool wb_wifi_is_connected(void);
bool wb_wifi_wait_connected(uint32_t timeout_ms);
wb_wifi_state_t wb_wifi_state(void);
const char *wb_wifi_state_name(wb_wifi_state_t state);
