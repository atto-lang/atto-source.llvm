#define _POSIX_C_SOURCE 200809L
#include "oreslang/runtime/NativeCallbackContext.h"
#include "oreslang/runtime/NativeLogicalTrace.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void check_impl(int ok, const char* expression, int line) {
    if (!ok) {
        fprintf(stderr, "CHECK_FAIL line=%d expr=%s\n", line, expression);
        abort();
    }
}
#define check(expression) check_impl((expression), #expression, __LINE__)

typedef struct {
    OresNativeExecutionContextStore* executions;
    OresNativeContinuationStore* continuations;
    OresNativeCallbackContext context;
    OresNativeCallbackProvenance provenance;
    int ok;
} WorkerArgs;

static void* observe_on_foreign_thread(void* raw) {
    WorkerArgs* args = (WorkerArgs*)raw;
    args->ok = ores_native_callback_context_snapshot(
        args->executions,
        args->continuations,
        &args->context,
        &args->provenance);
    return NULL;
}

int main(void) {
    OresNativeExecutionContextStore* executions =
        ores_native_execution_context_store_open(8);
    check(executions != NULL);
    OresNativeContinuationStore* continuations =
        ores_native_continuation_store_open(8, executions);
    check(continuations != NULL);

    OresNativeExecutionContext active = {
        .id = {17, 41},
        .allocation = {ORES_ALLOCATION_DOMAIN_ACTOR, 17, 3, 9},
        .parent_execution = {0, 0},
        .continuation = {0, 0}
    };
    OresNativeExecutionHandle execution = {0, 0};
    check(ores_native_execution_context_store_insert(
        executions, &active, &execution));

    OresNativeContinuationHandle continuation = {0, 0};
    check(ores_native_continuation_store_publish(
        continuations,
        execution,
        (OresNativeContinuationHandle){0, 0},
        ORES_CONTINUATION_BOUNDARY_AWAIT,
        4,
        9001,
        &continuation));
    check(ores_native_execution_context_store_bind_continuation(
        executions, execution, continuation));

    OresNativeCallbackContext callback = {0};
    check(ores_native_callback_context_capture(
        executions,
        continuations,
        execution,
        continuation,
        ORES_CALLBACK_BOUNDARY_IO,
        &callback));
    check(sizeof(callback) == 40);
    check(callback.source_execution_id.runtime_domain == 17);
    check(callback.source_execution_id.execution == 41);
    check(callback.boundary == ORES_CALLBACK_BOUNDARY_IO);

    OresNativeCallbackContext callback_before_overwrite = callback;
    check(!ores_native_callback_context_capture(
        executions,
        continuations,
        execution,
        continuation,
        ORES_CALLBACK_BOUNDARY_TIMER,
        &callback));
    check(memcmp(
        &callback,
        &callback_before_overwrite,
        sizeof(callback)) == 0);

    check(ores_native_continuation_store_release(continuations, continuation));
    check(ores_native_execution_context_store_release(executions, execution));
    check(ores_native_execution_context_store_live(executions) == 1);
    check(ores_native_continuation_store_live(continuations) == 1);

    WorkerArgs worker = {
        .executions = executions,
        .continuations = continuations,
        .context = callback,
        .ok = 0
    };
    pthread_t thread;
    check(pthread_create(&thread, NULL, observe_on_foreign_thread, &worker) == 0);
    check(pthread_join(thread, NULL) == 0);
    check(worker.ok);
    check(worker.provenance.source_execution_id.runtime_domain == 17);
    check(worker.provenance.source_execution_id.execution == 41);
    check(worker.provenance.boundary == ORES_CALLBACK_BOUNDARY_IO);

    OresNativeExecutionTraceFrame execution_frames[2];
    OresNativeContinuationTraceFrame continuation_frames[2];
    OresNativeCallbackLogicalTraceResult callback_trace = {0};
    check(ores_native_logical_trace_snapshot_callback(
        executions,
        continuations,
        &callback,
        execution_frames,
        2,
        continuation_frames,
        2,
        &callback_trace));
    check(callback_trace.callback.boundary == ORES_CALLBACK_BOUNDARY_IO);
    check(callback_trace.callback.source_execution_id.execution == 41);
    check(callback_trace.logical.execution_count == 1);
    check(callback_trace.logical.continuation_count == 1);
    check(!callback_trace.logical.corrupt);
    check(continuation_frames[0].record.boundary ==
        ORES_CONTINUATION_BOUNDARY_AWAIT);

    /* Callback trace snapshots are diagnostics, never allocation authority. */
    check(execution_frames[0].context.allocation.kind ==
        ORES_ALLOCATION_DOMAIN_NONE);
    check(execution_frames[0].context.allocation.domain == 0);
    check(execution_frames[0].context.allocation.owner == 0);
    check(execution_frames[0].context.allocation.generation == 0);
    check(continuation_frames[0].record.allocation.kind ==
        ORES_ALLOCATION_DOMAIN_NONE);
    check(continuation_frames[0].record.allocation.domain == 0);
    check(continuation_frames[0].record.allocation.owner == 0);
    check(continuation_frames[0].record.allocation.generation == 0);
    check((int)continuation_frames[0].record.state == 0);

    /*
     * Scheduler lifecycle changes after callback registration must not mutate
     * the callback diagnostic evidence. The retained causal record remains
     * addressable, but volatile continuation state is deliberately scrubbed.
     */
    OresNativeContinuationRecord claimed = {0};
    check(ores_native_continuation_store_try_claim_resume(
        continuations, callback.source_continuation, &claimed));
    check(claimed.state == ORES_CONTINUATION_CLAIMED);
    check(ores_native_continuation_store_finish_resume(
        continuations, callback.source_continuation));

    OresNativeExecutionTraceFrame execution_frames_after[2];
    OresNativeContinuationTraceFrame continuation_frames_after[2];
    OresNativeCallbackLogicalTraceResult callback_trace_after = {0};
    check(ores_native_logical_trace_snapshot_callback(
        executions,
        continuations,
        &callback,
        execution_frames_after,
        2,
        continuation_frames_after,
        2,
        &callback_trace_after));
    check(callback_trace_after.callback.boundary ==
        callback_trace.callback.boundary);
    check(callback_trace_after.callback.source_execution_id.execution ==
        callback_trace.callback.source_execution_id.execution);
    check(callback_trace_after.logical.execution_count ==
        callback_trace.logical.execution_count);
    check(callback_trace_after.logical.continuation_count ==
        callback_trace.logical.continuation_count);
    check(execution_frames_after[0].context.id.execution ==
        execution_frames[0].context.id.execution);
    check(execution_frames_after[0].context.parent_execution.slot ==
        execution_frames[0].context.parent_execution.slot);
    check(continuation_frames_after[0].record.boundary ==
        continuation_frames[0].record.boundary);
    check(continuation_frames_after[0].record.code_generation ==
        continuation_frames[0].record.code_generation);
    check(continuation_frames_after[0].record.source_token ==
        continuation_frames[0].record.source_token);
    check((int)continuation_frames_after[0].record.state == 0);
    check(continuation_frames_after[0].record.allocation.kind ==
        ORES_ALLOCATION_DOMAIN_NONE);

    OresNativeCallbackContext stale_copy = callback;
    check(ores_native_callback_context_release(
        executions, continuations, &callback));
    check(!ores_native_store_handle_valid(callback.source_execution));
    check(!ores_native_store_handle_valid(callback.source_continuation));
    check(ores_native_execution_context_store_live(executions) == 0);
    check(ores_native_continuation_store_live(continuations) == 0);

    OresNativeCallbackProvenance stale;
    memset(&stale, 0xa5, sizeof(stale));
    check(!ores_native_callback_context_snapshot(
        executions, continuations, &stale_copy, &stale));
    OresNativeCallbackProvenance zero_provenance = {0};
    check(memcmp(&stale, &zero_provenance, sizeof(stale)) == 0);
    OresNativeCallbackLogicalTraceResult stale_trace = {0};
    check(!ores_native_logical_trace_snapshot_callback(
        executions,
        continuations,
        &stale_copy,
        execution_frames,
        2,
        continuation_frames,
        2,
        &stale_trace));
    check(ores_native_callback_context_release(
        executions, continuations, &callback));

    OresNativeCallbackContext invalid = {0};
    check(!ores_native_callback_context_capture(
        executions,
        continuations,
        (OresNativeExecutionHandle){1, 0},
        (OresNativeContinuationHandle){0, 0},
        ORES_CALLBACK_BOUNDARY_HOST_CALLBACK,
        &invalid));
    check(!ores_native_callback_context_capture(
        executions,
        continuations,
        (OresNativeExecutionHandle){0, 0},
        (OresNativeContinuationHandle){0, 0},
        (OresNativeCallbackBoundary)99,
        &invalid));

    OresNativeCallbackContext forged_empty = {0};
    forged_empty.boundary = ORES_CALLBACK_BOUNDARY_HOST_CALLBACK;
    check(!ores_native_callback_context_release(
        executions, continuations, &forged_empty));

    check(ores_native_continuation_store_close(continuations));
    check(ores_native_execution_context_store_close(executions));
    puts("NATIVE_CALLBACK_CONTEXT_PROVENANCE_PASS");
    return 0;
}
