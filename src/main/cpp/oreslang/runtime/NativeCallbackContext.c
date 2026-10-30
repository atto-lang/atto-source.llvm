#include "oreslang/runtime/NativeCallbackContext.h"

#include <string.h>

static int same_handle(OresNativeStoreHandle left, OresNativeStoreHandle right) {
    return left.slot == right.slot && left.generation == right.generation;
}

static int same_execution_id(
        OresNativeExecutionId left, OresNativeExecutionId right) {
    return left.runtime_domain == right.runtime_domain
        && left.execution == right.execution;
}

static int boundary_valid(OresNativeCallbackBoundary boundary) {
    return boundary >= ORES_CALLBACK_BOUNDARY_HOST_CALLBACK
        && boundary <= ORES_CALLBACK_BOUNDARY_SPAWN;
}

int ores_native_callback_context_capture(
        OresNativeExecutionContextStore* executions,
        OresNativeContinuationStore* continuations,
        OresNativeExecutionHandle source_execution,
        OresNativeContinuationHandle source_continuation,
        OresNativeCallbackBoundary boundary,
        OresNativeCallbackContext* out) {
    if (!executions || !continuations || !out || !boundary_valid(boundary)
            || !ores_native_store_handle_valid(source_execution))
        return 0;

    OresNativeExecutionContext execution = {0};
    if (!ores_native_execution_context_store_get(
            executions, source_execution, &execution))
        return 0;

    if (ores_native_store_handle_valid(source_continuation)) {
        OresNativeContinuationRecord record = {0};
        if (!ores_native_continuation_store_get(
                continuations, source_continuation, &record)
                || !same_handle(record.execution, source_execution)
                || !same_execution_id(record.execution_id, execution.id))
            return 0;
    } else if (source_continuation.slot != 0
            || source_continuation.generation != 0) {
        return 0;
    }

    if (!ores_native_execution_context_store_retain(
            executions, source_execution))
        return 0;

    if (ores_native_store_handle_valid(source_continuation)
            && !ores_native_continuation_store_retain(
                continuations, source_continuation)) {
        (void)ores_native_execution_context_store_release(
            executions, source_execution);
        return 0;
    }

    out->source_execution_id = execution.id;
    out->source_execution = source_execution;
    out->source_continuation = source_continuation;
    out->boundary = (uint32_t)boundary;
    out->flags = 0;
    return 1;
}

int ores_native_callback_context_snapshot(
        OresNativeExecutionContextStore* executions,
        OresNativeContinuationStore* continuations,
        const OresNativeCallbackContext* context,
        OresNativeCallbackProvenance* out) {
    if (!executions || !continuations || !context || !out
            || !boundary_valid((OresNativeCallbackBoundary)context->boundary)
            || context->flags != 0
            || !ores_native_store_handle_valid(context->source_execution))
        return 0;

    OresNativeExecutionContext execution = {0};
    if (!ores_native_execution_context_store_get(
            executions, context->source_execution, &execution)
            || !same_execution_id(execution.id, context->source_execution_id))
        return 0;

    if (ores_native_store_handle_valid(context->source_continuation)) {
        OresNativeContinuationRecord record = {0};
        if (!ores_native_continuation_store_get(
                continuations, context->source_continuation, &record)
                || !same_handle(record.execution, context->source_execution)
                || !same_execution_id(record.execution_id, execution.id))
            return 0;
    } else if (context->source_continuation.slot != 0
            || context->source_continuation.generation != 0) {
        return 0;
    }

    out->source_execution_id = context->source_execution_id;
    out->source_execution = context->source_execution;
    out->source_continuation = context->source_continuation;
    out->boundary = (OresNativeCallbackBoundary)context->boundary;
    return 1;
}

int ores_native_callback_context_release(
        OresNativeExecutionContextStore* executions,
        OresNativeContinuationStore* continuations,
        OresNativeCallbackContext* context) {
    if (!executions || !continuations || !context) return 0;
    if (!ores_native_store_handle_valid(context->source_execution)) {
        const int empty = context->source_execution.slot == 0
            && context->source_execution.generation == 0
            && context->source_continuation.slot == 0
            && context->source_continuation.generation == 0;
        if (empty) return 1;
        return 0;
    }

    const OresNativeExecutionHandle execution = context->source_execution;
    const OresNativeContinuationHandle continuation =
        context->source_continuation;
    memset(context, 0, sizeof(*context));

    int ok = 1;
    if (ores_native_store_handle_valid(continuation)
            && !ores_native_continuation_store_release(
                continuations, continuation))
        ok = 0;
    if (!ores_native_execution_context_store_release(executions, execution))
        ok = 0;
    return ok;
}
