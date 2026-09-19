// What a loaded game is allowed to reach by name.
//
// Everything about the CONSOLE arrives through the api table, so nothing of the console is
// here. What is here is the C library: a game that computes a sine or formats a string has
// to get sinf and snprintf from somewhere, and the alternative - a copy of libm inside
// every package - would make each one an order of magnitude bigger for no gain.
//
// These names are safe to freeze in a way console APIs are not. sinf has meant the same
// thing for forty years and will mean it in forty more, so a package built today keeps
// working against a firmware built much later.
//
// The list started as exactly what the seven built-in games needed, which turned out to be
// the wrong way to size it: the first game written fresh against the API reached for
// memmove and would not load. A list drawn from what today's games happen to use is a list
// that is already too short for tomorrow's.
//
// So it is the ordinary C library a small game can reasonably expect, whether or not
// anything here uses it yet. Adding costs a few bytes of table and breaks nothing; taking
// something away breaks every package that used it, so don't.
#include "exports.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// The compiler emits calls to these itself for arithmetic the chip has no instruction for
// - dividing floats, widening to double - so they have to be here even though no game
// mentions them.
extern float __divsf3(float, float);
extern long long __divdi3(long long, long long);
extern unsigned long long __udivdi3(unsigned long long, unsigned long long);
extern long long __moddi3(long long, long long);
extern unsigned long long __umoddi3(unsigned long long, unsigned long long);
extern double __extendsfdf2(float);
extern float __truncdfsf2(double);
extern float __floatdisf(long long);
extern int __fixsfsi(float);
extern float __floatsisf(int);

#define EXPORT(name) {#name, (const void *)(name)}

static const loader_export_t s_exports[] = {
    /* compiler helpers, which the compiler emits without anybody asking for them */
    EXPORT(__divsf3), EXPORT(__divdi3), EXPORT(__udivdi3), EXPORT(__moddi3), EXPORT(__umoddi3),
    EXPORT(__extendsfdf2), EXPORT(__truncdfsf2), EXPORT(__floatdisf), EXPORT(__fixsfsi),
    EXPORT(__floatsisf),

    /* maths */
    EXPORT(sinf), EXPORT(cosf), EXPORT(tanf), EXPORT(asinf), EXPORT(acosf), EXPORT(atanf),
    EXPORT(atan2f), EXPORT(sqrtf), EXPORT(hypotf), EXPORT(expf), EXPORT(logf), EXPORT(log10f),
    EXPORT(powf), EXPORT(fmodf), EXPORT(floorf), EXPORT(ceilf), EXPORT(roundf), EXPORT(truncf),
    EXPORT(fabsf), EXPORT(fminf), EXPORT(fmaxf), EXPORT(lroundf), EXPORT(copysignf), EXPORT(ldexpf),

    /* strings and memory */
    EXPORT(memset), EXPORT(memcpy), EXPORT(memmove), EXPORT(memcmp), EXPORT(memchr),
    EXPORT(strlen), EXPORT(strcmp), EXPORT(strncmp), EXPORT(strcpy), EXPORT(strncpy),
    EXPORT(strcat), EXPORT(strchr), EXPORT(strstr), EXPORT(snprintf),

    /* the odd bits of stdlib a game reaches for */
    EXPORT(abs), EXPORT(labs),
};

const void *loader_lookup_export(const char *name)
{
    for (size_t i = 0; i < sizeof(s_exports) / sizeof(s_exports[0]); i++)
        if (strcmp(s_exports[i].name, name) == 0) return s_exports[i].addr;
    return NULL;
}
