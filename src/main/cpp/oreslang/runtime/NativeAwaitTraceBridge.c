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

static int resume_begin_checked(
        OresNativeExecutionContextStore* executions,
        OresNativeContinuationStore* continuations,
        OresNativeExecutionId resumed_execution_id,
        OresNativeSuspensionLink link,
        OresNativeExecutionHandle* out_execution,
        OresNativeExecutionContext* out_context,
        int requires_ready) {
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
        /*
         * continuation is OUTGOING from this execution. The incoming boundary
         * is link.continuation, owned by link.parent and recoverable through
         * parent execution ancestry.
         */
        .continuation = {0, 0}
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
    const int claim_ok = requires_ready
        ? ores_native_continuation_store_try_claim_ready_resume(
            continuations, link.continuation, &claimed)
        : ores_native_continuation_store_try_claim_resume(
            continuations, link.continuation, &claimed);
    if (!claim_ok) {
        (void)ores_native_execution_context_store_release(
            executions, execution);
        return 0;
    }

    *out_execution = execution;
    if (out_context) *out_context = context;
    return 1;
}

int ores_native_await_trace_resume_begin(
        OresNativeExecutionContextStore* executions,
        OresNativeContinuationStore* continuations,
        OresNativeExecutionId resumed_execution_id,
        OresNativeSuspensionLink link,
        OresNativeExecutionHandle* out_execution,
        OresNativeExecutionContext* out_context) {
    /* Direct host trace bootstrap, not a permission to wake an unresolved Future. */
    return resume_begin_checked(executions, continuations,
        resumed_execution_id, link, out_execution, out_context, 0);
}

int ores_native_await_trace_ready_resume_begin(
        OresNativeExecutionContextStore* executions,
        OresNativeContinuationStore* continuations,
        OresNativeExecutionId resumed_execution_id,
        OresNativeSuspensionLink link,
        OresNativeExecutionHandle* out_execution,
        OresNativeExecutionContext* out_context) {
    /* A scheduler wake must never claim PENDING or CANCELLED work. */
    return resume_begin_checked(executions, continuations,
        resumed_execution_id, link, out_execution, out_context, 1);
}

int ores_native_await_trace_resume_finish(
        OresNativeContinuationStore* continuations,
        OresNativeSuspensionLink link) {
    if (!continuations || !ores_native_store_handle_valid(link.continuation)
            || !ores_native_store_handle_valid(link.parent))
        return 0;

    /*
     * Only the exact suspended execution may finish its physical resume.
     * Reject forged/recycled parent generations and logical execution IDs,
     * instead of treating the continuation handle alone as authorization.
     */
    OresNativeContinuationRecord record;
    if (!ores_native_continuation_store_get(
            continuations, link.continuation, &record)
            || !same_handle(record.execution, link.parent)
            || !same_execution_id(record.execution_id, link.execution_id))
        return 0;

    return ores_native_continuation_store_finish_resume(
        continuations, link.continuation);
}
