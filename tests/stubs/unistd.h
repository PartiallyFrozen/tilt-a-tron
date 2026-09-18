#pragma once
#include <stdio.h>
static inline int tat_unlink(const char *p) { return remove(p); }
#define unlink tat_unlink
static inline int rmdir(const char *p) { (void)p; return 0; }
