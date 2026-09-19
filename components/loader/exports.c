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
// The list is exactly what the seven built-in games need, found by compiling each one and
// taking the union of its undefined symbols. Adding to it is cheap and breaks nothing;
// taking something away would break every package that used it, so don't.
#include "exports.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

// The compiler emits calls to these itself for arithmetic the chip has no instruction for
// - dividing floats, widening to double - so they have to be here even though no game
// mentions them.
extern float __divsf3(float, float);
extern long long __divdi3(long long, long long);
extern double __extendsfdf2(float);
extern float __floatdisf(long long);

#define EXPORT(name) {#name, (const void *)(name)}

static const loader_export_t s_exports[] = {
    /* compiler helpers */
    EXPORT(__divsf3), EXPORT(__divdi3), EXPORT(__extendsfdf2), EXPORT(__floatdisf),

    /* maths */
    EXPORT(sinf), EXPORT(cosf), EXPORT(sqrtf), EXPORT(atan2f), EXPORT(expf), EXPORT(powf),
    EXPORT(fmodf), EXPORT(hypotf), EXPORT(floorf), EXPORT(ceilf), EXPORT(fmaxf), EXPORT(lroundf),

    /* strings and memory */
    EXPORT(memset), EXPORT(memcpy), EXPORT(strlen), EXPORT(strcat), EXPORT(snprintf),
};

const void *loader_lookup_export(const char *name)
{
    for (size_t i = 0; i < sizeof(s_exports) / sizeof(s_exports[0]); i++)
        if (strcmp(s_exports[i].name, name) == 0) return s_exports[i].addr;
    return NULL;
}
