#include "oreslang/runtime/NativeLogicalTrace.h"

#include <string.h>

int ores_native_logical_trace_snapshot(
        OresNativeExecutionContextStore* executions,
        OresNativeContinuationStore* continuations,
        OresNativeExecutionHandle current_execution,
        OresNativeExecutionTraceFrame* execution_frames,
        size_t execution_capacity,
        OresNativeContinuationTraceFrame* continuation_frames,
        size_t continuation_capacity,
        OresNativeLogicalTraceResult* out) {
    if (!executions || !continuations || !out
            || !ores_native_store_handle_valid(current_execution)
            || (execution_capacity != 0 && !execution_frames)
            || (continuation_capacity != 0 && !continuation_frames))
        return 0;

    memset(out, 0, sizeof(*out));

    OresNativeExecutionHandle execution = current_execution;
    OresNativeContinuationHandle continuation = {0, 0};
    const uint32_t execution_limit =
        ores_native_execution_context_store_capacity(executions);
    const uint32_t continuation_limit =
        ores_native_continuation_store_capacity(continuations);
    uint32_t execution_steps = 0;

    while (ores_native_store_handle_valid(execution)
            && execution_steps < execution_limit) {
        ++execution_steps;
        OresNativeExecutionContext context;
        if (!ores_native_execution_context_store_get(
                executions, execution, &context)) {
            out->corrupt = 1;
            break;
        }

        /*
         * continuation is the outgoing suspension created by an execution.
         * A newly resumed current execution has no outgoing continuation yet,
         * so walk toward its parent until the incoming boundary is found.
         * Keep discovering even after the caller's execution buffer fills:
         * a tiny execution rendering budget must not hide causal continuations.
         */
        if (!ores_native_store_handle_valid(continuation)
                && ores_native_store_handle_valid(context.continuation))
            continuation = context.continuation;

        if (out->execution_count < execution_capacity) {
            execution_frames[out->execution_count].handle = execution;
            execution_frames[out->execution_count].context = context;
            ++out->execution_count;
        } else {
            out->execution_truncated = 1;
        }
        execution = context.parent_execution;
    }
    if (ores_native_store_handle_valid(execution))
        out->corrupt = 1; /* impossible without a cycle/corrupt parent chain */

    uint32_t continuation_steps = 0;
    while (ores_native_store_handle_valid(continuation)
            && continuation_steps < continuation_limit) {
        ++continuation_steps;
        OresNativeContinuationRecord record;
        if (!ores_native_continuation_store_get(
                continuations, continuation, &record)) {
            out->corrupt = 1;
            break;
        }
        if (out->continuation_count < continuation_capacity) {
            continuation_frames[out->continuation_count].handle = continuation;
            continuation_frames[out->continuation_count].record = record;
            ++out->continuation_count;
        } else {
            out->continuation_truncated = 1;
        }
        continuation = record.parent;
    }
    if (ores_native_store_handle_valid(continuation))
        out->corrupt = 1; /* impossible without a cycle/corrupt parent chain */
    return 1;
}
