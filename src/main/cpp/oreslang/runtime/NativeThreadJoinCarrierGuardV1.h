#ifndef ORESLANG_NATIVE_THREAD_JOIN_CARRIER_GUARD_V1_H
#define ORESLANG_NATIVE_THREAD_JOIN_CARRIER_GUARD_V1_H

#include "oreslang/runtime/NativeActorScheduler.h"
#include "oreslang/runtime/NativeThreadV1.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Host-only, synchronous pthread join. The compiler must never lower guest
 * Task/actor join_blocking to this function. A scheduler-aware guest await
 * instead subscribes to the bounded join reaper and parks the logical Task.
 *
 * This guard checks ANY active scheduler callback, not merely the scheduler
 * which created the native thread. It does not consume ownership or write
 * *out_result on denial. Calls on standalone OS threads may block physically.
 *
 * This checks the native actor scheduler carrier only; a future unrelated
 * Task executor MUST implement its own carrier admission fence as well.
 */
static inline ores_thread_status_v1 ores_native_thread_join_blocking_checked_v1(
        ores_native_thread_v1* handle, int* out_result) {
    if (!handle || !out_result) return ORES_THREAD_BAD_ARGUMENT_V1;
    if (ores_native_actor_scheduler_is_any_current_carrier())
        return ORES_THREAD_CARRIER_WOULD_BLOCK_V1;
    return ores_native_thread_join_blocking_v1(handle, out_result);
}

#ifdef __cplusplus
}
#endif
#endif
