#ifndef ORESLANG_NATIVE_STRING_REGION_H
#define ORESLANG_NATIVE_STRING_REGION_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Private compiler ABI. Handles cannot cross root invocation/thread boundaries.
 * begin/end belong to the OS wrapper, never guest actor/control callbacks.
 * Every string is an explicit (data, size) span. size is authoritative,
 * embedded NUL bytes are ordinary data, and strlen/sentinel semantics are
 * forbidden for Oreslang string values.
 */
int64_t ores_string_region_begin(int64_t console);
int64_t ores_string_region_end(void);
int64_t ores_string_literal(const char *data, int64_t size);
int64_t ores_string_concat(int64_t left, int64_t right);
int64_t ores_string_equal(int64_t left, int64_t right);
int64_t ores_string_print(int64_t handle, int64_t newline);
/* Text of a scalar exactly as Java stdio prints it, as a new owned string. */
int64_t ores_string_literal_interned(const char *data, int64_t size);
int64_t ores_string_print_i64(int64_t value, int64_t newline);
int64_t ores_string_print_f64(double value, int64_t newline);
int64_t ores_string_from_i64(int64_t value);
int64_t ores_string_from_f64(double value);
/* Flushes admitted output, reports the message, exits with status 1. */
void ores_rt_fail(int64_t handle);
#ifdef __cplusplus
}
#endif
#endif
