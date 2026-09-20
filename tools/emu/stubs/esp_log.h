// Logging goes to stdout, tagged the way the watch's /log is, so a run reads the same.
#pragma once
#include <stdio.h>
#ifdef __cplusplus
extern "C" {
#endif
void emu_log_line(char level, const char *tag, const char *fmt, ...);
#ifdef __cplusplus
}
#endif
#define ESP_LOGE(tag, ...) emu_log_line('E', tag, __VA_ARGS__)
#define ESP_LOGW(tag, ...) emu_log_line('W', tag, __VA_ARGS__)
#define ESP_LOGI(tag, ...) emu_log_line('I', tag, __VA_ARGS__)
#define ESP_LOGD(tag, ...) ((void)0)
