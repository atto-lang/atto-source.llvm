#pragma once

#include "oreslang/runtime/NativeContinuationStore.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Suspension-side lowering hook. It persists the execution id, newly minted
 * continuation handle and future parent-execution handle. No worker stack
 * address enters the persisted link.
 */
int ores_native_await_trace_suspend(
    OresNativeExecutionContextStore* executions,
    OresNativeContinuationStore* continuations,
    OresNativeExecutionHandle current_execution,
    OresNativeContinuationHandle parent_continuation,
    OresNativeContinuationBoundary boundary,
    uint64_t code_generation,
    uint64_t source_token,
    OresNativeSuspensionLink* out_link);

/*
 * Resume-side lowering hook. The scheduler supplies a fresh logical ExecutionId
 * for the resumed turn. The actor/task allocation capability is inherited from
 * the suspended continuation; it is never derived from the new execution id.
 */
int ores_native_await_trace_resume_begin(
    OresNativeExecutionContextStore* executions,
    OresNativeContinuationStore* continuations,
    OresNativeExecutionId resumed_execution_id,
    OresNativeSuspensionLink link,
    OresNativeExecutionHandle* out_execution,
    OresNativeExecutionContext* out_context);

/*
 * Scheduler-only Future wakeup path. Unlike the legacy direct causal trace
 * resume, this refuses to resume until the owning producer has published
 * READY in NativeContinuationStore, and atomically competes with cancellation.
 * Call only after the scheduler admitted the correct actor CONTROL turn.
 */
int ores_native_await_trace_ready_resume_begin(
    OresNativeExecutionContextStore* executions,
    OresNativeContinuationStore* continuations,
    OresNativeExecutionId resumed_execution_id,
    OresNativeSuspensionLink link,
    OresNativeExecutionHandle* out_execution,
    OresNativeExecutionContext* out_context);

/*
 * Finish after the resumed guest turn/cleanup has left its native stack.
 * The suspended continuation and its exact generation-fenced parent handle
 * and logical source ExecutionId must still match the stored claim. This is
 * a trusted scheduler operation, not an exposed guest completion capability.
 */
int ores_native_await_trace_resume_finish(
    OresNativeContinuationStore* continuations,
    OresNativeSuspensionLink link);

#ifdef __cplusplus
}
#endif
