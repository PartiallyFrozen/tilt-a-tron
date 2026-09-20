#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
uint32_t esp_random(void);   // seeded, so a scripted run repeats; --seed changes it
#ifdef __cplusplus
}
#endif
