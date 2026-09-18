// A test harness small enough that it needs no explaining and no dependency.
//
//   CHECK(safe_path("a/b", 3, out, sizeof out));
//   CHECK_STR(out, "/data/a/b");
//
// Every file ends with `return failures();`.
#pragma once

#include <stdio.h>
#include <string.h>

static int tat_checks, tat_failed;

#define CHECK(cond)                                                              \
    do {                                                                         \
        tat_checks++;                                                            \
        if (!(cond)) {                                                           \
            tat_failed++;                                                        \
            printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);             \
        }                                                                        \
    } while (0)

#define CHECK_EQ(got, want)                                                      \
    do {                                                                         \
        tat_checks++;                                                            \
        const long long g_ = (long long)(got), w_ = (long long)(want);           \
        if (g_ != w_) {                                                          \
            tat_failed++;                                                        \
            printf("  FAIL %s:%d  %s: got %lld, wanted %lld\n",                  \
                   __FILE__, __LINE__, #got, g_, w_);                            \
        }                                                                        \
    } while (0)

#define CHECK_STR(got, want)                                                     \
    do {                                                                         \
        tat_checks++;                                                            \
        if (strcmp((got), (want)) != 0) {                                        \
            tat_failed++;                                                        \
            printf("  FAIL %s:%d  got \"%s\", wanted \"%s\"\n",                  \
                   __FILE__, __LINE__, (got), (want));                           \
        }                                                                        \
    } while (0)

static inline int failures(const char *name)
{
    printf("%s: %d checks, %d failed\n", name, tat_checks, tat_failed);
    return tat_failed != 0;
}
