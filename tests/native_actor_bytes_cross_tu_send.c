#include "oreslang/runtime/NativeActorBytesSchedulerBinding.h"

/* This helper is deliberately compiled as another translation unit. A
 * static-inline function address in a header cannot be the identity
 * of a per-actor transport across independent producer modules. */
OresNativeSendStatus ores_native_actor_bytes_test_send_cross_tu(
    const OresNativeActorBytesSchedulerBinding* binding,
    uint64_t payload, const void* bytes, size_t length) {
    return ores_native_actor_bytes_try_send_atomic(
        binding, payload, bytes, length);
}
