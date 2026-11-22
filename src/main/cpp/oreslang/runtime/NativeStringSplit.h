#ifndef ORESLANG_NATIVE_STRING_SPLIT_H
#define ORESLANG_NATIVE_STRING_SPLIT_H
#include "oreslang/runtime/NativeIntArrayRegion.h"
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Fill one freshly created empty, uniquely owned Array<String> using
 * length-delimited byte separators. Returns exact checked resource status.
 * No actor authority or raw string pointer crosses the guest ABI.
 */
OresNativeIntArrayStatus ores_string_split_literal(
    OresNativeIntArrayRegion *arrays, uint64_t destination,
    int64_t source, int64_t separator);
#ifdef __cplusplus
}
#endif
#endif
