/* Floating point conversions with x86 semantics (out-of-range results produce the "integer indefinite" value
 * 0x80000000 as the hardware does, instead of C undefined behaviour). */
#ifndef PROTORE_RECOMP_RUNTIME_FP_H
#define PROTORE_RECOMP_RUNTIME_FP_H

#include <math.h>
#include <stdint.h>

static inline int32_t RecompTruncF32(float v) {
    if (!(v > -2147483649.0f && v < 2147483648.0f)) {
        return INT32_MIN;
    }
    return (int32_t)v;
}

/* x87 store with the default round-to-nearest-even control word. */
static inline int32_t RecompRoundToInt(double v) {
    double r = nearbyint(v);
    if (!(r >= -2147483648.0 && r <= 2147483647.0)) {
        return INT32_MIN;
    }
    return (int32_t)r;
}

static inline int64_t RecompRoundToInt64(double v) {
    double r = nearbyint(v);
    if (!(r >= -9223372036854775808.0 && r < 9223372036854775808.0)) {
        return INT64_MIN;
    }
    return (int64_t)r;
}

#endif
