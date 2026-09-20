// Time is the emulator's own: it advances by exactly the step it is given, so a scripted
// run is the same run every time, whatever the PC is doing.
#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
int64_t esp_timer_get_time(void);
#ifdef __cplusplus
}
#endif
