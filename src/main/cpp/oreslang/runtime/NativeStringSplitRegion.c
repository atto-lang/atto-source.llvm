/*
 * Compiler-only bridge from immutable invocation-owned String spans to one
 * existing, unique Array<String> owner. No guest-facing raw pointers.
 *
 * The array is created and its shadow owner registered by LLVM codegen
 * before entering this function. A nonzero status MUST route through the
 * compiler's array_failure path, which drops partial array contents, closes
 * array region and reports the resource failure; callers may not keep
 * partially filled arrays.
 */
#include "oreslang/runtime/NativeStringRegion.h"
#include "oreslang/runtime/NativeStringSplitRegion.h"
#include <stdint.h>
#include <stddef.h>
#include <string.h>

enum { MAX_LITERAL_SPLIT_TOKENS = 1024, MAX_LITERAL_DELIMITER_BYTES = 64 };
// LLVM codegen currently emits the bridge return ABI as i32. Never let a
// compiler flag or target ABI silently change the C status width.
_Static_assert(sizeof(OresNativeIntArrayStatus) == sizeof(int32_t),
    "native String split status must preserve LLVM i32 ABI");

OresNativeIntArrayStatus ores_string_split_literal_fill(
    int64_t source, int64_t delimiter, OresNativeIntArrayRegion *region,
    uint64_t destination) {
    // The destination is a freshly created, exclusively owned EMPTY
    // array. Validate its native handle and emptiness before looking up
    // Strings or publishing any new views. A stale/cross-region handle
    // aborts through the existing array ownership boundary; a valid
    // nonempty owner is rejected unchanged rather than being appended to.
    if (oreslang_int_array_length(region, destination) != 0)
        return ORES_NATIVE_ARRAY_LIMIT;
    const char *bytes = NULL, *separator = NULL;
    uint64_t size = 0, sep_size = 0;
    if (ores_string_region_borrow_span(source, &bytes, &size) ||
            ores_string_region_borrow_span(delimiter, &separator, &sep_size))
        return ORES_NATIVE_ARRAY_LIMIT; /* corrupted String authority */
    if (!sep_size || sep_size > MAX_LITERAL_DELIMITER_BYTES)
        return ORES_NATIVE_ARRAY_LIMIT; /* source compiler must reject */
    /* A nonempty delimiter is always sourced from a String literal. */
    if (!separator || (size && !bytes))
        return ORES_NATIVE_ARRAY_LIMIT;
    uint64_t offset = 0, count = 0;
    for (;;) {
        uint64_t end = size;
        if (size - offset >= sep_size) {
            const size_t last = (size_t)(size - sep_size);
            const char first = separator[0];
            for (size_t at = (size_t)offset; at <= last;) {
                const char *match = memchr(bytes + at, (unsigned char)first,
                                          last - at + 1);
                if (!match) break;
                at = (size_t)(match - bytes);
                if (memcmp(match, separator, (size_t)sep_size) == 0) {
                    end = at;
                    break;
                }
                ++at;
            }
        }
        if (++count > MAX_LITERAL_SPLIT_TOKENS)
            return ORES_NATIVE_ARRAY_LIMIT;
        const int64_t token = ores_string_region_publish_slice(
            source, offset, end - offset);
        if (!token) return ORES_NATIVE_ARRAY_NO_MEMORY;
        const OresNativeIntArrayStatus status = oreslang_int_array_try_add(
            region, destination, token);
        if (status != ORES_NATIVE_ARRAY_OK) return status;
        if (end == size) return ORES_NATIVE_ARRAY_OK;
        offset = end + sep_size; /* end <= size - sep_size */
    }
}
