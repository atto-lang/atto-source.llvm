#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/*
 * Trusted, single-owner native Array<int> storage substrate.
 *
 * This is NOT yet Oreslang source lowering, a cross-thread shared collection,
 * an unforgeable capability, or a remotely callable API. Every operation is
 * performed on one serialized owner turn. Invalid handles, bounds, quota,
 * and token exhaustion fail closed by aborting. Resource quotas return
 * statuses through checked APIs but abort via legacy entry points. The original APIs
 * also abort on resource exhaustion; checked compiler-only try_* APIs
 * instead distinguish quota and allocation failure without mutating state.
 *
 * Monotonically unique process-local handles never recycle, even when an
 * array slot or region is dropped and recreated. No raw element pointer
 * escapes this API. Region close requires prior explicit destruction.
 */
typedef struct OresNativeIntArrayRegion OresNativeIntArrayRegion;
typedef uint64_t OresNativeIntArrayHandle;

/* Success=0; quota/size exhaustion and out-of-memory are recoverable.
 * Invalid, stale or cross-region handles remain fatal ownership violations.
 * On error, output arguments and existing live array data are unchanged.
 */
typedef enum {
    ORES_NATIVE_ARRAY_OK = 0,
    ORES_NATIVE_ARRAY_LIMIT = 1,
    ORES_NATIVE_ARRAY_NO_MEMORY = 2
} OresNativeIntArrayStatus;

OresNativeIntArrayStatus oreslang_int_array_try_region_open(
    OresNativeIntArrayRegion** out_region);
OresNativeIntArrayStatus oreslang_int_array_try_new(
    OresNativeIntArrayRegion* region, OresNativeIntArrayHandle* out_handle);
OresNativeIntArrayStatus oreslang_int_array_try_reserve(
    OresNativeIntArrayRegion* region, OresNativeIntArrayHandle handle, uint64_t capacity);
OresNativeIntArrayStatus oreslang_int_array_try_add(
    OresNativeIntArrayRegion* region, OresNativeIntArrayHandle handle, int64_t value);

/* Prepare n independent, initially empty owned array handles. No output is
 * published until ALL preparations succeed. Partial creation is rolled back
 * in reverse order, leaving pre-existing region arrays untouched.
 * The caller supplies at least n writable handle slots.
 */
OresNativeIntArrayStatus oreslang_int_array_try_prepare_many(
    OresNativeIntArrayRegion* region, uint64_t n, OresNativeIntArrayHandle* out);

/* Transactionally prepare n fully initialized arrays, each with exactly
 * lengths[i] elements initially equal to values[i]. Arrays reserve precisely
 * their declared length (rather than repeated geometric append growth).
 * Validate the aggregate budget BEFORE allocating; if any new/reserve fails,
 * destroy all staged owners and zeroize their contents, leaving existing
 * arrays and the caller's out[] completely unchanged.
 * Caller supplies valid, nonaliased n-element lengths[], values[], out[].
 * This is a trusted compiler ABI, not a guest-callable operation.
 */
OresNativeIntArrayStatus oreslang_int_array_try_prepare_filled_many(
    OresNativeIntArrayRegion* region, uint64_t n,
    const uint64_t lengths[], const int64_t values[],
    OresNativeIntArrayHandle out[]);

/* Single-owner, allocation-free, all-or-nothing ownership transfer from n
 * distinct staged source places into n distinct destination owner places.
 * Validates every token and alias relation before mutating any place. Sources
 * are disarmed (0) after commit, old destination arrays dropped/zeroized.
 * destinations[i] and sources[i] must be writable handle-slot pointers.
 * Stale/aliased/foreign tokens or duplicate slot addresses abort.
 */
void oreslang_int_array_commit_many(OresNativeIntArrayRegion* region, uint64_t n,
    OresNativeIntArrayHandle* const destinations[],
    OresNativeIntArrayHandle* const sources[]);

OresNativeIntArrayRegion* oreslang_int_array_region_open(void);
OresNativeIntArrayHandle oreslang_int_array_new(OresNativeIntArrayRegion* region);
void oreslang_int_array_reserve(OresNativeIntArrayRegion* region, OresNativeIntArrayHandle handle, uint64_t capacity);
void oreslang_int_array_add(OresNativeIntArrayRegion* region, OresNativeIntArrayHandle handle, int64_t value);
uint64_t oreslang_int_array_length(OresNativeIntArrayRegion* region, OresNativeIntArrayHandle handle);
int64_t oreslang_int_array_get(OresNativeIntArrayRegion* region, OresNativeIntArrayHandle handle, uint64_t index);
void oreslang_int_array_set(OresNativeIntArrayRegion* region, OresNativeIntArrayHandle handle, uint64_t index, int64_t value);
void oreslang_int_array_drop(OresNativeIntArrayRegion* region, OresNativeIntArrayHandle handle);
void oreslang_int_array_region_close(OresNativeIntArrayRegion* region);

#ifdef ORESLANG_NATIVE_TEST_TOKEN_EXHAUSTION
/* Test-only hook, intentionally absent from production builds. */
void oreslang_int_array_test_set_next_token(uint64_t token);
#endif
#ifdef ORESLANG_NATIVE_TEST_ALLOC_FAILURE
/* Test-only deterministic fault injector; -1 disables injection.
 * 0 fails the next region/new/reserve physical or logical allocation attempt,
 * 1 fails the attempt after that, and so forth; never compiled in production.
 */
void oreslang_int_array_test_fail_after(int64_t attempts);
/* Test-only audit counter for backing-buffer bytes wiped before release.
 * It excludes metadata wiping and resets without modifying owners.
 */
void oreslang_int_array_test_reset_scrubbed_backing_bytes(void);
uint64_t oreslang_int_array_test_scrubbed_backing_bytes(void);
/* Deliberately corrupt one valid live array's metadata for child-process
 * fail-closed tests. Not linked into production or exposed to Oreslang.
 */
void oreslang_int_array_test_corrupt_length(
    OresNativeIntArrayRegion* region, OresNativeIntArrayHandle handle,
    uint64_t impossible_length);
/* Test-only corruption of region aggregate counters; verifies no unsafe
 * abandonment, quota bypass or new owner publication with forged metadata.
 */
void oreslang_int_array_test_corrupt_region_accounting(
    OresNativeIntArrayRegion* region, uint64_t live, uint64_t reserved);
#endif
#ifdef __cplusplus
}
#endif
