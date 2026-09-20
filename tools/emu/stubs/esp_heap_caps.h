// The emulator's stand-ins for ESP-IDF. Just enough for the console's own sources to build
// on a PC unchanged; see tools/emu/README.md.
#pragma once
#include <stdlib.h>
#define MALLOC_CAP_SPIRAM 0
#define MALLOC_CAP_INTERNAL 0
#define MALLOC_CAP_8BIT 0
#define MALLOC_CAP_DMA 0
static inline void *heap_caps_malloc(size_t n, int caps) { (void)caps; return malloc(n); }
static inline void *heap_caps_calloc(size_t n, size_t s, int caps) { (void)caps; return calloc(n, s); }
static inline void heap_caps_free(void *p) { free(p); }
static inline size_t heap_caps_get_free_size(int caps) { (void)caps; return 8u << 20; }
static inline size_t heap_caps_get_largest_free_block(int caps) { (void)caps; return 4u << 20; }
