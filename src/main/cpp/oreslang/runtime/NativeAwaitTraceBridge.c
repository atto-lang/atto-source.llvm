#include "oreslang/runtime/NativeAwaitTraceBridge.h"

static int same_handle(OresNativeStoreHandle a, OresNativeStoreHandle b) {
    return a.slot == b.slot && a.generation == b.generation;
}

static int same_execution_id(OresNativeExecutionId a, OresNativeExecutionId b) {
    return a.runtime_domain == b.runtime_domain && a.execution == b.execution;
}

int ores_native_await_trace_suspend(
        OresNativeExecutionContextStore* executions,
        OresNativeContinuationStore* continuations,
        OresNativeExecutionHandle current_execution,
        OresNativeContinuationHandle parent_continuation,
        OresNativeContinuationBoundary boundary,
        uint64_t code_generation,
        uint64_t source_token,
        OresNativeSuspensionLink* out_link) {
    if (!executions || !continuations || !out_link
            || !ores_native_store_handle_valid(current_execution))
        return 0;

    OresNativeExecutionContext context;
    if (!ores_native_execution_context_store_get(
            executions, current_execution, &context))
        return 0;

    OresNativeContinuationHandle continuation = {0, 0};
    if (!ores_native_continuation_store_publish(
            continuations,
            current_execution,
            parent_continuation,
            boundary,
            code_generation,
            source_token,
            &continuation))
        return 0;

    if (!ores_native_execution_context_store_bind_continuation(
            executions, current_execution, continuation)) {
        (void)ores_native_continuation_store_release(
            continuations, continuation);
        return 0;
    }

    out_link->execution_id = context.id;
    out_link->continuation = continuation;
    /* The suspended execution becomes the resumed execution's parent. */
    out_link->parent = current_execution;
    return 1;
}

int ores_native_await_trace_resume_begin(
        OresNativeExecutionContextStore* executions,
        OresNativeContinuationStore* continuations,
        OresNativeExecutionId resumed_execution_id,
        OresNativeSuspensionLink link,
        OresNativeExecutionHandle* out_execution,
        OresNativeExecutionContext* out_context) {
    if (!executions || !continuations || !out_execution
            || resumed_execution_id.runtime_domain == 0
            || resumed_execution_id.execution == 0
            || !ores_native_store_handle_valid(link.continuation)
            || !ores_native_store_handle_valid(link.parent))
        return 0;

    OresNativeContinuationRecord record;
    if (!ores_native_continuation_store_get(
            continuations, link.continuation, &record)
            || !same_handle(record.execution, link.parent)
            || !same_execution_id(record.execution_id, link.execution_id)
            || resumed_execution_id.runtime_domain
                != link.execution_id.runtime_domain)
        return 0;

    OresNativeExecutionContext context = {
        .id = resumed_execution_id,
        .allocation = record.allocation,
        .parent_execution = link.parent,
        .continuation = link.continuation
    };
    OresNativeExecutionHandle execution = {0, 0};
    if (!ores_native_execution_context_store_insert(
            executions, &context, &execution))
        return 0;

    /*
     * Insert first, then claim. A racing loser releases its unpublished child,
     * which also drops the retained parent; only the claim winner survives.
     */
    OresNativeContinuationRecord claimed;
    if (!ores_native_continuation_store_try_claim_resume(
            continuations, link.continuation, &claimed)) {
        (void)ores_native_execution_context_store_release(
            executions, execution);
        return 0;
    }

    *out_execution = execution;
    if (out_context) *out_context = context;
    return 1;
}

int ores_native_await_trace_resume_finish(
        OresNativeContinuationStore* continuations,
        OresNativeSuspensionLink link) {
    if (!continuations || !ores_native_store_handle_valid(link.continuation))
        return 0;
    return ores_native_continuation_store_finish_resume(
        continuations, link.continuation);
}
