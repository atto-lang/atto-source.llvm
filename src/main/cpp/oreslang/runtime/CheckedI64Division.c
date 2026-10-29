#include "oreslang/runtime/CheckedI64Division.h"

#include <limits.h>
#include <stdint.h>

OresI64DivStatus oreslang_i64_div_trunc_checked(
        int64_t numerator, int64_t denominator, int64_t* out_quotient) {
    if (out_quotient == 0) return ORES_I64_DIV_BAD_OUTPUT;
    if (denominator == 0) return ORES_I64_DIV_ZERO;
    /* INT64_MIN / -1 overflows signed i64 and is undefined in C and poison
     * in LLVM; check both operands BEFORE issuing machine integer divide. */
    if (numerator == INT64_MIN && denominator == -1)
        return ORES_I64_DIV_OVERFLOW;
    const int64_t quotient = numerator / denominator;
    *out_quotient = quotient;
    return ORES_I64_DIV_OK;
}
