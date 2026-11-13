#pragma once
#include "oreslang/runtime/NativeIntArrayRegion.h"
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Trusted compiler-only String.split_literal -> uniquely-owned Array<String>.
 * Destination must be a live, exclusive empty array owner. The bridge
 * validates this before publishing any String views: an invalid handle
 * aborts; an already-populated owner returns LIMIT unchanged. Caller drops
 * partially filled array on any non-OK resource status.
 * No native pointer/array handle may escape a String invocation region.
 */
OresNativeIntArrayStatus ores_string_split_literal_fill(
    int64_t source, int64_t delimiter,
    OresNativeIntArrayRegion *region, uint64_t destination);
#ifdef __cplusplus
}
#endif
