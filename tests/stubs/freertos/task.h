#pragma once
#include "freertos/FreeRTOS.h"
typedef void *TaskHandle_t;
static inline void vTaskDelay(TickType_t t) { (void)t; }
static inline BaseType_t xTaskCreatePinnedToCore(void (*f)(void *), const char *n, uint32_t s,
                                                 void *p, int pr, TaskHandle_t *h, int core)
{ (void)f; (void)n; (void)s; (void)p; (void)pr; (void)h; (void)core; return pdPASS; }
