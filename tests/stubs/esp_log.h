// Just enough ESP-IDF to compile firmware logic on a PC. See tests/README.md.
#pragma once
#include <stdio.h>
#define ESP_LOGE(tag, ...) ((void)0)
#define ESP_LOGW(tag, ...) ((void)0)
#define ESP_LOGI(tag, ...) ((void)0)
#define ESP_LOGD(tag, ...) ((void)0)
