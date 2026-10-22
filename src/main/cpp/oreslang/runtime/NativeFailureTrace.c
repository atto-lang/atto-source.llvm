#include "oreslang/runtime/NativeFailureTrace.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static int valid_kind(OresNativeFailureTraceKind kind) {
    return kind >= ORES_FAILURE_TRACE_THROW
        && kind <= ORES_FAILURE_TRACE_FATAL;
}

const char* ores_native_failure_trace_kind_name(
        OresNativeFailureTraceKind kind) {
    switch (kind) {
        case ORES_FAILURE_TRACE_THROW: return "throw";
        case ORES_FAILURE_TRACE_RAISE: return "raise";
        case ORES_FAILURE_TRACE_PANIC: return "panic";
        case ORES_FAILURE_TRACE_CANCEL: return "cancel";
        case ORES_FAILURE_TRACE_FATAL: return "fatal";
    }
    return "invalid";
}

static const char* allocation_name(uint64_t kind) {
    switch (kind) {
        case ORES_ALLOCATION_DOMAIN_NONE: return "none";
        case ORES_ALLOCATION_DOMAIN_ACTOR: return "actor";
        case ORES_ALLOCATION_DOMAIN_TASK: return "task";
        case ORES_ALLOCATION_DOMAIN_RUNTIME: return "runtime";
    }
    return "invalid";
}

static const char* boundary_name(OresNativeContinuationBoundary boundary) {
    switch (boundary) {
        case ORES_CONTINUATION_BOUNDARY_AWAIT: return "await";
        case ORES_CONTINUATION_BOUNDARY_SPAWN: return "spawn";
        case ORES_CONTINUATION_BOUNDARY_ACTOR_MESSAGE: return "actor-message";
        case ORES_CONTINUATION_BOUNDARY_SUPERVISOR_CONTROL:
            return "supervisor-control";
        case ORES_CONTINUATION_BOUNDARY_TASK_JOIN: return "task-join";
    }
    return "invalid";
}

static const char* continuation_state_name(OresNativeContinuationState state) {
    switch (state) {
        case ORES_CONTINUATION_PENDING: return "pending";
        case ORES_CONTINUATION_CLAIMED: return "claimed";
        case ORES_CONTINUATION_COMPLETED: return "completed";
        case ORES_CONTINUATION_CANCELLED: return "cancelled";
    }
    return "invalid";
}

int ores_native_failure_trace_capture(
        OresNativeExecutionContextStore* executions,
        OresNativeContinuationStore* continuations,
        OresNativeExecutionHandle current_execution,
        OresNativeFailureTraceKind kind,
        uint32_t code,
        OresNativeExecutionTraceFrame* execution_frames,
        size_t execution_capacity,
        OresNativeContinuationTraceFrame* continuation_frames,
        size_t continuation_capacity,
        OresNativeFailureTraceReport* out) {
    if (!executions || !continuations || !out || !valid_kind(kind)
            || !ores_native_store_handle_valid(current_execution))
        return 0;

    /*
     * The current execution is authoritative, unlike a missing older ancestry
     * node which the snapshot reports as logical.corrupt. Pin it across the
     * multi-lock snapshot so a concurrent owner release cannot turn a valid
     * failure site into a stale-root success.
     */
    if (!ores_native_execution_context_store_retain(
            executions, current_execution))
        return 0;

    OresNativeFailureTraceReport result = {0};
    result.kind = kind;
    result.code = code;
    result.current_execution = current_execution;
    const int captured = ores_native_logical_trace_snapshot(
        executions,
        continuations,
        current_execution,
        execution_frames,
        execution_capacity,
        continuation_frames,
        continuation_capacity,
        &result.logical);
    const int released = ores_native_execution_context_store_release(
        executions, current_execution);
    if (!captured || !released)
        return 0;
    *out = result;
    return 1;
}

typedef struct {
    char* buffer;
    size_t capacity;
    size_t written;
    size_t required;
} TextWriter;

static void append_bytes(TextWriter* writer, const char* text, size_t length) {
    if (writer->required <= SIZE_MAX - length)
        writer->required += length;
    else
        writer->required = SIZE_MAX;

    if (!writer->buffer || writer->capacity == 0
            || writer->written >= writer->capacity - 1)
        return;
    const size_t available = writer->capacity - 1 - writer->written;
    const size_t copy = length < available ? length : available;
    memcpy(writer->buffer + writer->written, text, copy);
    writer->written += copy;
}

static int append_format(TextWriter* writer, const char* format, ...) {
    char line[384];
    va_list args;
    va_start(args, format);
    const int count = vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    if (count < 0 || (size_t)count >= sizeof(line)) return 0;
    append_bytes(writer, line, (size_t)count);
    return 1;
}

int ores_native_failure_trace_render(
        OresNativeFailureTraceReport* report,
        const OresNativeExecutionTraceFrame* execution_frames,
        const OresNativeContinuationTraceFrame* continuation_frames,
        char* buffer,
        size_t capacity) {
    if (!report || !valid_kind(report->kind)
            || (report->logical.execution_count && !execution_frames)
            || (report->logical.continuation_count && !continuation_frames)
            || (!buffer && capacity != 0))
        return 0;

    TextWriter writer = {buffer, capacity, 0, 0};
    if (buffer && capacity) buffer[0] = '\0';

    if (!append_format(&writer,
            "%s code=%" PRIu32 " execution=%" PRIu32 "@%" PRIu32 "\n",
            ores_native_failure_trace_kind_name(report->kind),
            report->code,
            report->current_execution.slot,
            report->current_execution.generation))
        return 0;

    for (size_t i = 0; i < report->logical.execution_count; ++i) {
        const OresNativeExecutionTraceFrame* frame = &execution_frames[i];
        const OresNativeExecutionContext* context = &frame->context;
        if (!append_format(&writer,
                "  execution runtime=%" PRIu64 " id=%" PRIu64
                " handle=%" PRIu32 "@%" PRIu32
                " allocation=%s:%" PRIu64 "/%" PRIu64 "@%" PRIu64 "\n",
                context->id.runtime_domain,
                context->id.execution,
                frame->handle.slot,
                frame->handle.generation,
                allocation_name(context->allocation.kind),
                context->allocation.domain,
                context->allocation.owner,
                context->allocation.generation))
            return 0;
    }
    if (report->logical.execution_truncated)
        append_bytes(&writer, "  ... execution ancestry truncated\n",
                     sizeof("  ... execution ancestry truncated\n") - 1);

    for (size_t i = 0; i < report->logical.continuation_count; ++i) {
        const OresNativeContinuationTraceFrame* frame = &continuation_frames[i];
        const OresNativeContinuationRecord* record = &frame->record;
        if (!append_format(&writer,
                "  boundary %s source=%" PRIu64 " codegen=%" PRIu64
                " state=%s origin=%" PRIu64 ":%" PRIu64
                " handle=%" PRIu32 "@%" PRIu32 "\n",
                boundary_name(record->boundary),
                record->source_token,
                record->code_generation,
                continuation_state_name(record->state),
                record->execution_id.runtime_domain,
                record->execution_id.execution,
                frame->handle.slot,
                frame->handle.generation))
            return 0;
    }
    if (report->logical.continuation_truncated)
        append_bytes(&writer, "  ... continuation ancestry truncated\n",
                     sizeof("  ... continuation ancestry truncated\n") - 1);
    if (report->logical.corrupt)
        append_bytes(&writer, "  ... logical ancestry corrupt\n",
                     sizeof("  ... logical ancestry corrupt\n") - 1);

    if (buffer && capacity) {
        const size_t end =
            writer.written < capacity ? writer.written : capacity - 1;
        buffer[end] = '\0';
    }
    report->rendered_bytes = writer.written;
    report->required_bytes = writer.required;
    report->text_truncated =
        capacity != 0 && writer.required > writer.written;
    return 1;
}
