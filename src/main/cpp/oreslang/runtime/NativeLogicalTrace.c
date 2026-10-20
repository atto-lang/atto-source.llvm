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
    while (ores_native_store_handle_valid(execution)) {
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
         */
        if (!ores_native_store_handle_valid(continuation)
                && ores_native_store_handle_valid(context.continuation))
            continuation = context.continuation;
        if (out->execution_count == execution_capacity) {
            out->execution_truncated = 1;
            break;
        }
        execution_frames[out->execution_count].handle = execution;
        execution_frames[out->execution_count].context = context;
        ++out->execution_count;
        execution = context.parent_execution;
    }

    while (ores_native_store_handle_valid(continuation)) {
        OresNativeContinuationRecord record;
        if (!ores_native_continuation_store_get(
                continuations, continuation, &record)) {
            out->corrupt = 1;
            break;
        }
        if (out->continuation_count == continuation_capacity) {
            out->continuation_truncated = 1;
            break;
        }
        continuation_frames[out->continuation_count].handle = continuation;
        continuation_frames[out->continuation_count].record = record;
        ++out->continuation_count;
        continuation = record.parent;
    }
    return 1;
}
