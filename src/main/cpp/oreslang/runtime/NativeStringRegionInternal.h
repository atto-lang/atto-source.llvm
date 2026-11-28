#ifndef ORESLANG_NATIVE_STRING_REGION_INTERNAL_H
#define ORESLANG_NATIVE_STRING_REGION_INTERNAL_H
/* Compiler/runtime-private helper surface. Never expose raw String bytes to
 * guest code, actors, FFI or an independently scheduled worker. Both helpers
 * require the caller to run on the same invocation-owned String region.
 */
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef enum {
    ORES_STRING_VIEWS_OK = 0,
    ORES_STRING_VIEWS_LIMIT = 1,
    ORES_STRING_VIEWS_NO_MEMORY = 2
} OresStringViewsStatus;
/* A returned view may only be inspected synchronously while the current
 * String region remains active and unmodified by independent threads.
 * Invalid handles poison the region and return 0.
 */
int ores_string_region_view(int64_t handle, const char **bytes, size_t *length);
/* Atomically append n non-owning slices of one live String. All bounds,
 * byte/count budgets, and backing metadata allocation are validated BEFORE
 * any new handle is published. On nonzero status out_handles[] and visible
 * String region entries are unchanged. Invalid authority is fatal.
 */
OresStringViewsStatus ores_string_region_publish_views(
    int64_t source, const size_t starts[], const size_t lengths[],
    size_t count, int64_t out_handles[]);
#ifdef __cplusplus
}
#endif
#endif
