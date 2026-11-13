#include "oreslang/runtime/NativeActorBytesSchedulerBinding.h"

/* Deliberately compiled as a separate C translation unit. A header-local
 * static inline commit callback would have a different function address here
 * than in the binding call site. Only the linked callback identity works. */
OresNativeSendStatus ores_native_actor_bytes_test_send_cross_tu(
    const OresNativeActorBytesSchedulerBinding* binding,
    uint64_t payload, const void* bytes, size_t length) {
    return ores_native_actor_bytes_try_send_atomic(
        binding, payload, bytes, length);
}
