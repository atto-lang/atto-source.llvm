#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Native signed-i64 quotient primitive. This is a pure host/compiler runtime
 * contract, NOT Oreslang source '/' (which yields floating point).
 *
 * Truncation is toward zero. Invalid arithmetic returns a separate status,
 * not a guessed success value, machine signal, integer wrap or LLVM poison.
 * On failure, *out_quotient is never modified. No heap allocation is used.
 * The compiler must check the result and map it to the language's Error/
 * Result effect before source-level promotion; that lowering is not yet
 * supplied by this ABI.
 */
typedef enum {
    ORES_I64_DIV_OK = 0,
    ORES_I64_DIV_ZERO = 1,
    ORES_I64_DIV_OVERFLOW = 2,
    ORES_I64_DIV_BAD_OUTPUT = 3
} OresI64DivStatus;

OresI64DivStatus oreslang_i64_div_trunc_checked(
    int64_t numerator, int64_t denominator, int64_t* out_quotient);

#ifdef __cplusplus
}
#endif
