#ifndef ORESLANG_NATIVE_STRING_REGION_H
#define ORESLANG_NATIVE_STRING_REGION_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Private compiler ABI. Handles cannot cross root invocation/thread boundaries.
 * begin/end belong to the OS wrapper, never guest actor/control callbacks.
 */
int64_t ores_string_region_begin(int64_t console);
int64_t ores_string_region_end(void);
int64_t ores_string_literal(const char *data, int64_t size);
int64_t ores_string_concat(int64_t left, int64_t right);
int64_t ores_string_print(int64_t handle, int64_t newline);
#ifdef __cplusplus
}
#endif
#endif
