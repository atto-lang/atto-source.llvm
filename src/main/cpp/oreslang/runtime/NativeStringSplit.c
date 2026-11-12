/* Bounded same-invocation String.split_literal -> owned Array<String>.
 * This is runtime plumbing, NOT a NATS protocol parser or actor transport.
 * The exact receiver/needle types and output owner are compiler-checked.
 */
#include "oreslang/runtime/NativeIntArrayRegion.h"
#include "oreslang/runtime/NativeStringRegionInternal.h"
#include "oreslang/runtime/NativeStringSplit.h"
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum { MAX_SPLIT_TOKENS = 65536 };
static void invalid(void) { abort(); }

/* Literal bytes are separators, not regex syntax. One final (possibly
 * empty) token is always produced. Multiple-byte separators do not overlap.
 */
static int at_separator(const char *source, size_t length,
                        const char *separator, size_t separator_length,
                        size_t index) {
    return separator_length <= length - index &&
        memcmp(source + index, separator, separator_length) == 0;
}

OresNativeIntArrayStatus ores_string_split_literal(
        OresNativeIntArrayRegion *arrays,
        uint64_t destination, int64_t source_handle, int64_t separator_handle) {
    if (!arrays || !destination) invalid();
    /* Require compiler-owned, brand-new empty array; never append tokens to
     * an existing subscriber's authenticated subscription collection.
     */
    if (oreslang_int_array_length(arrays, destination) != 0) invalid();
    const char *source = NULL, *separator = NULL;
    size_t source_length = 0, separator_length = 0;
    if (!ores_string_region_view(source_handle, &source, &source_length) ||
        !ores_string_region_view(separator_handle, &separator, &separator_length))
        invalid();
    /* An empty separator has no bounded literal-token semantics here.
     * Fail closed rather than guessing Java String.split_literal behavior.
     */
    if (separator_length == 0) invalid();

    size_t count = 1;
    size_t index = 0;
    while (index < source_length && separator_length <= source_length - index) {
        if (at_separator(source, source_length, separator, separator_length, index)) {
            if (++count > MAX_SPLIT_TOKENS) return ORES_NATIVE_ARRAY_LIMIT;
            index += separator_length;
        } else {
            ++index;
        }
    }
    /* count is bounded and cannot overflow size_t in the scratch plan. */
    size_t *offsets = malloc(count * sizeof(*offsets));
    size_t *sizes = malloc(count * sizeof(*sizes));
    int64_t *handles = malloc(count * sizeof(*handles));
    if (!offsets || !sizes || !handles) {
        free(offsets); free(sizes); free(handles);
        return ORES_NATIVE_ARRAY_NO_MEMORY;
    }
    size_t token = 0, start = 0;
    index = 0;
    while (index < source_length && separator_length <= source_length - index) {
        if (at_separator(source, source_length, separator, separator_length, index)) {
            offsets[token] = start;
            sizes[token++] = index - start;
            index += separator_length;
            start = index;
        } else {
            ++index;
        }
    }
    offsets[token] = start;
    sizes[token++] = source_length - start;
    if (token != count) invalid();

    /* Checked array reserve is the only potentially allocating array
     * operation. On failure, no String handles or array elements are
     * committed. The compiler's normal failure epilogue drops destination.
     */
    OresNativeIntArrayStatus status =
        oreslang_int_array_try_reserve(arrays, destination, (uint64_t)count);
    if (status != ORES_NATIVE_ARRAY_OK) goto done;
    OresStringViewsStatus published = ores_string_region_publish_views(
        source_handle, offsets, sizes, count, handles);
    if (published != ORES_STRING_VIEWS_OK) {
        status = published == ORES_STRING_VIEWS_LIMIT
            ? ORES_NATIVE_ARRAY_LIMIT : ORES_NATIVE_ARRAY_NO_MEMORY;
        goto done;
    }
    /* The array already owns at least count reserved slots, so each append
     * cannot allocate or fail except on internal invariant corruption.
     * Never expose a partially initialized array to guest execution.
     */
    for (size_t i = 0; i < count; ++i)
        if (oreslang_int_array_try_add(arrays, destination, handles[i])
                != ORES_NATIVE_ARRAY_OK)
            invalid();
done:
    free(offsets); free(sizes); free(handles);
    return status;
}
