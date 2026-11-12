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
/* Binary-safe, no-GC String.contains_literal; operates on bounded UTF-8 byte spans. */
int64_t ores_string_contains_literal(int64_t haystack, int64_t needle);
/* Trusted compiler/runtime-only span borrow and same-region string view.
 * Lifetime ends at ores_string_region_end(); never expose to guest/actors. */
int32_t ores_string_region_borrow_span(int64_t handle, const char **data, uint64_t *size);
int64_t ores_string_region_publish_slice(int64_t source, uint64_t start, uint64_t size);
int64_t ores_string_from_i64(int64_t value);
int64_t ores_string_from_bool(int64_t value);
int64_t ores_string_print(int64_t handle, int64_t newline);
/* Panic path only: drain queued console output before the process aborts.
 * No-op when no console region is active. */
void ores_string_panic_flush(void);
#ifdef __cplusplus
}
#endif
#endif
