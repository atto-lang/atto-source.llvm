#include "oreslang/runtime/CheckedI64Division.h"

#include <inttypes.h>
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define CHECK(expr) do { if (!(expr)) { \
    fprintf(stderr, "checked-i64 quotient failure line %d: %s\\n", __LINE__, #expr); \
    exit(1); \
} } while (0)

/* Independent unsigned-magnitude oracle: negating signed INT64_MIN is never
 * attempted. This is separate from the production signed "/" implementation.
 */
static uint64_t magnitude(int64_t x) {
    uint64_t bits = (uint64_t)x;
    return x < 0 ? (~bits + UINT64_C(1)) : bits;
}

static OresI64DivStatus oracle(int64_t a, int64_t b, int64_t* out) {
    if (!out) return ORES_I64_DIV_BAD_OUTPUT;
    if (!b) return ORES_I64_DIV_ZERO;
    if (a == INT64_MIN && b == -1) return ORES_I64_DIV_OVERFLOW;
    uint64_t q = magnitude(a) / magnitude(b);
    const int negative = (a < 0) != (b < 0);
    if (negative) {
        if (q == (UINT64_C(1) << 63)) *out = INT64_MIN;
        else *out = -(int64_t)q;
    } else {
        CHECK(q <= (uint64_t)INT64_MAX);
        *out = (int64_t)q;
    }
    return ORES_I64_DIV_OK;
}

static void compare_case(int64_t a, int64_t b) {
    const int64_t sentinel = INT64_C(0x1234123412341234);
    int64_t actual = sentinel, reference = sentinel;
    const OresI64DivStatus expected = oracle(a, b, &reference);
    const OresI64DivStatus got =
        oreslang_i64_div_trunc_checked(a, b, &actual);
    if (got != expected || actual != reference) {
        fprintf(stderr, "mismatch: a=%" PRId64 " b=%" PRId64
                " status=%d expected=%d actual=%" PRId64
                " reference=%" PRId64 "\\n",
                a, b, (int)got, (int)expected, actual, reference);
        exit(1);
    }
}

static uint64_t rng_state = UINT64_C(0xD1B54A32D192ED03);
static uint64_t next_u64(void) {
    uint64_t x = rng_state;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    rng_state = x;
    return x * UINT64_C(2685821657736338717);
}

int main(void) {
    const int64_t boundaries[] = {
        INT64_MIN, INT64_MIN + 1, -INT64_C(9223372036854775807),
        -INT64_C(1000000), -42, -3, -2, -1, 0,
        1, 2, 3, 7, 42, 1000000, INT64_MAX - 1, INT64_MAX
    };
    const size_t n = sizeof(boundaries) / sizeof(boundaries[0]);
    CHECK(n == 17);
    for (size_t i = 0; i < n; ++i) {
        for (size_t j = 0; j < n; ++j)
            compare_case(boundaries[i], boundaries[j]);
    }
    /* No output pointer is also an ordinary non-UB status, not a crash. */
    CHECK(oreslang_i64_div_trunc_checked(42, 2, NULL)
          == ORES_I64_DIV_BAD_OUTPUT);
    {
        int64_t output = 93;
        CHECK(oreslang_i64_div_trunc_checked(42, 0, &output)
              == ORES_I64_DIV_ZERO && output == 93);
        CHECK(oreslang_i64_div_trunc_checked(INT64_MIN, -1, &output)
              == ORES_I64_DIV_OVERFLOW && output == 93);
        CHECK(oreslang_i64_div_trunc_checked(INT64_MIN, 1, &output)
              == ORES_I64_DIV_OK && output == INT64_MIN);
        CHECK(oreslang_i64_div_trunc_checked(-7, 3, &output)
              == ORES_I64_DIV_OK && output == -2);
        CHECK(oreslang_i64_div_trunc_checked(7, -3, &output)
              == ORES_I64_DIV_OK && output == -2);
        CHECK(oreslang_i64_div_trunc_checked(-7, -3, &output)
              == ORES_I64_DIV_OK && output == 2);
    }

    /* Deterministic, full-width and intentionally degenerate randomized
     * signed input distributions. Zero/one/-one and MIN appear frequently. */
    for (uint32_t i = 0; i < 200000; ++i) {
        int64_t a = (int64_t)next_u64();
        int64_t b = (int64_t)next_u64();
        switch (i % 16) {
            case 0: b = 0; break;
            case 1: b = -1; break;
            case 2: b = 1; break;
            case 3: a = INT64_MIN; break;
            case 4: a = INT64_MAX; break;
            case 5: b = INT64_MIN; break;
            case 6: a = 0; break;
            case 7: b = 2; break;
            case 8: b = -2; break;
            default: break;
        }
        compare_case(a, b);
    }
    puts("CHECKED-I64-DIV-HOST-ONLY-OK cases=200289");
    return 0;
}
