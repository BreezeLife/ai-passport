#ifndef LV_CONF_H
#define LV_CONF_H

/* Deliberately matches the device's 48 KiB pool and RGB565 software renderer.
   Host pointers are wider, so pool pressure is at least as strict as the board. */
#define LV_COLOR_DEPTH 16
#define LV_USE_STDLIB_MALLOC LV_STDLIB_BUILTIN
#define LV_MEM_SIZE (48 * 1024U)
#define LV_USE_OS LV_OS_NONE
#define LV_USE_DRAW_SW 1
#define LV_DRAW_SW_DRAW_UNIT_CNT 1
#define LV_DRAW_SW_COMPLEX 1
#define LV_DRAW_BUF_STRIDE_ALIGN 1
#define LV_DRAW_BUF_ALIGN 4
#define LV_USE_LOG 1
#define LV_LOG_LEVEL LV_LOG_LEVEL_WARN
#define LV_LOG_PRINTF 0
#define LV_USE_ASSERT_NULL 1
#define LV_USE_ASSERT_MALLOC 1
#define LV_USE_ASSERT_MEM_INTEGRITY 1
#define LV_ASSERT_HANDLER_INCLUDE <stdlib.h>
#define LV_ASSERT_HANDLER abort();
#define LV_USE_FONT_PLACEHOLDER 1
#define LV_USE_SYSMON 0
#define LV_USE_PERF_MONITOR 0
#define LV_BUILD_EXAMPLES 0
#define LV_BUILD_DEMOS 0

#endif
