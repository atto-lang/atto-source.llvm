#include "oreslang/runtime/NativeFailureTrace.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static int valid_kind(OresNativeFailureKind kind) {
    return kind == ORES_NATIVE_FAILURE_THROW
        || kind == ORES_NATIVE_FAILURE_RAISE
        || kind == ORES_NATIVE_FAILURE_PANIC;
}

static const char* kind_name(OresNativeFailureKind kind) {
    switch (kind) {
        case ORES_NATIVE_FAILURE_THROW: return "THROW";
        case ORES_NATIVE_FAILURE_RAISE: return "RAISE";
        case ORES_NATIVE_FAILURE_PANIC: return "PANIC";
    }
    return "INVALID";
}

static const char* boundary_name(OresNativeContinuationBoundary boundary) {
    switch (boundary) {
        case ORES_CONTINUATION_BOUNDARY_AWAIT: return "AWAIT";
        case ORES_CONTINUATION_BOUNDARY_SPAWN: return "SPAWN";
        case ORES_CONTINUATION_BOUNDARY_ACTOR_MESSAGE: return "ACTOR_MESSAGE";
        case ORES_CONTINUATION_BOUNDARY_SUPERVISOR_CONTROL:
            return "SUPERVISOR_CONTROL";
        case ORES_CONTINUATION_BOUNDARY_TASK_JOIN: return "TASK_JOIN";
    }
    return "UNKNOWN";
}

int ores_native_failure_trace_capture(
        OresNativeExecutionContextStore* executions,
        OresNativeContinuationStore* continuations,
        OresNativeExecutionHandle current_execution,
        OresNativeFailureKind kind,
        uint64_t source_token,
        OresNativeExecutionTraceFrame* execution_frames,
        size_t execution_capacity,
        OresNativeContinuationTraceFrame* continuation_frames,
        size_t continuation_capacity,
        OresNativeFailureTraceHeader* out) {
    if (out) memset(out, 0, sizeof(*out));
    if (!executions || !continuations || !out || !valid_kind(kind)
            || !ores_native_store_handle_valid(current_execution))
        return 0;

    if (!ores_native_execution_context_store_retain(
            executions, current_execution))
        return 0;

    OresNativeExecutionContext context;
    int ok = ores_native_execution_context_store_get(
        executions, current_execution, &context);
    if (ok) {
        out->kind = kind;
        out->source_token = source_token;
        out->current_execution = current_execution;
        out->current_execution_id = context.id;
        out->allocation = context.allocation;
        ok = ores_native_logical_trace_snapshot(
            executions,
            continuations,
            current_execution,
            execution_frames,
            execution_capacity,
            continuation_frames,
            continuation_capacity,
            &out->logical);
    }

    if (!ores_native_execution_context_store_release(
            executions, current_execution) || !ok) {
        memset(out, 0, sizeof(*out));
        return 0;
    }
    return 1;
}

typedef struct {
    char* buffer;
    size_t capacity;
    size_t used;
} TraceWriter;

static void appendf(TraceWriter* writer, const char* format, ...) {
    if (!writer || writer->capacity == 0 || writer->used >= writer->capacity)
        return;
    va_list args;
    va_start(args, format);
    const size_t available = writer->capacity - writer->used;
    int written = vsnprintf(
        writer->buffer + writer->used, available, format, args);
    va_end(args);
    if (written < 0) {
        writer->buffer[writer->used] = '\0';
        return;
    }
    const size_t amount = (size_t)written;
    if (amount >= available)
        writer->used = writer->capacity - 1;
    else
        writer->used += amount;
}

size_t ores_native_failure_trace_format_checked(
        const OresNativeFailureTraceHeader* header,
        const OresNativeExecutionTraceFrame* execution_frames,
        size_t execution_capacity,
        const OresNativeContinuationTraceFrame* continuation_frames,
        size_t continuation_capacity,
        char* buffer,
        size_t capacity) {
    if (buffer && capacity != 0) buffer[0] = '\0';
    if (!header || !valid_kind(header->kind) || (!buffer && capacity != 0)
            || header->logical.execution_count > execution_capacity
            || header->logical.continuation_count > continuation_capacity
            || (header->logical.execution_count != 0 && !execution_frames)
            || (header->logical.continuation_count != 0 && !continuation_frames))
        return 0;
    if (capacity == 0) return 0;
    buffer[0] = '\0';

    TraceWriter writer = {buffer, capacity, 0};
    appendf(&writer,
        "%s source=%llu execution=%llu:%llu allocation=%llu:%llu:%llu:%llu\n",
        kind_name(header->kind),
        (unsigned long long)header->source_token,
        (unsigned long long)header->current_execution_id.runtime_domain,
        (unsigned long long)header->current_execution_id.execution,
        (unsigned long long)header->allocation.kind,
        (unsigned long long)header->allocation.domain,
        (unsigned long long)header->allocation.owner,
        (unsigned long long)header->allocation.generation);

    for (size_t i = 0; i < header->logical.execution_count
            && writer.used < writer.capacity - 1; ++i) {
        const OresNativeExecutionContext* context =
            &execution_frames[i].context;
        appendf(&writer,
            "  execution[%llu] id=%llu:%llu handle=%u:%u parent=%u:%u\n",
            (unsigned long long)i,
            (unsigned long long)context->id.runtime_domain,
            (unsigned long long)context->id.execution,
            execution_frames[i].handle.slot,
            execution_frames[i].handle.generation,
            context->parent_execution.slot,
            context->parent_execution.generation);
    }
    if (header->logical.execution_truncated)
        appendf(&writer, "  ... execution ancestry truncated ...\n");

    for (size_t i = 0; i < header->logical.continuation_count
            && writer.used < writer.capacity - 1; ++i) {
        const OresNativeContinuationRecord* record =
            &continuation_frames[i].record;
        appendf(&writer,
            "  boundary[%llu] %s source=%llu code_generation=%llu continuation=%u:%u\n",
            (unsigned long long)i,
            boundary_name(record->boundary),
            (unsigned long long)record->source_token,
            (unsigned long long)record->code_generation,
            continuation_frames[i].handle.slot,
            continuation_frames[i].handle.generation);
    }
    if (header->logical.continuation_truncated)
        appendf(&writer, "  ... continuation ancestry truncated ...\n");
    if (header->logical.corrupt)
        appendf(&writer, "  ... logical ancestry corrupt ...\n");

    return writer.used;
}

size_t ores_native_failure_trace_format(
        const OresNativeFailureTraceHeader* header,
        const OresNativeExecutionTraceFrame* execution_frames,
        const OresNativeContinuationTraceFrame* continuation_frames,
        char* buffer,
        size_t capacity) {
    /* Compatibility API: the caller already guarantees captured frame counts.
     * New consumers must supply actual buffer bounds through the checked API. */
    return ores_native_failure_trace_format_checked(
        header, execution_frames, header ? header->logical.execution_count : 0,
        continuation_frames, header ? header->logical.continuation_count : 0,
        buffer, capacity);
}
