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
 * allocation failure, and token exhaustion fail closed by aborting.
 *
 * Monotonically unique process-local handles never recycle, even when an
 * array slot or region is dropped and recreated. No raw element pointer
 * escapes this API. Region close requires prior explicit destruction.
 */
typedef struct OresNativeIntArrayRegion OresNativeIntArrayRegion;
typedef uint64_t OresNativeIntArrayHandle;

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
#ifdef __cplusplus
}
#endif
