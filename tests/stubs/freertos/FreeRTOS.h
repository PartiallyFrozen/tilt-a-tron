#pragma once
#include <stdint.h>
typedef uint32_t TickType_t;
typedef int BaseType_t;
#define pdMS_TO_TICKS(ms) (ms)
#define pdPASS 1
#define portMAX_DELAY 0xFFFFFFFF
#define configMAX_PRIORITIES 25
