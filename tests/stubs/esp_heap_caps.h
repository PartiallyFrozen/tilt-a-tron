#pragma once
#include <stdlib.h>
#define MALLOC_CAP_SPIRAM 0
#define MALLOC_CAP_INTERNAL 0
#define MALLOC_CAP_8BIT 0
static inline void *heap_caps_malloc(size_t n, int caps) { (void)caps; return malloc(n); }
static inline void *heap_caps_calloc(size_t n, size_t s, int caps) { (void)caps; return calloc(n, s); }
static inline void heap_caps_free(void *p) { free(p); }
