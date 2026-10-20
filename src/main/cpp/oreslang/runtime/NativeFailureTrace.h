#pragma once

#include "oreslang/runtime/NativeLogicalTrace.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ORES_FAILURE_TRACE_THROW = 1,
    ORES_FAILURE_TRACE_RAISE = 2,
    ORES_FAILURE_TRACE_PANIC = 3,
    ORES_FAILURE_TRACE_CANCEL = 4,
    ORES_FAILURE_TRACE_FATAL = 5
} OresNativeFailureTraceKind;

typedef struct {
    OresNativeFailureTraceKind kind;
    uint32_t code;
    OresNativeExecutionHandle current_execution;
    OresNativeLogicalTraceResult logical;
    size_t rendered_bytes;
    size_t required_bytes;
    int text_truncated;
} OresNativeFailureTraceReport;

/* Capture logical ancestry from a generation-checked active execution handle.
 * No allocation occurs and no native stack pointer is retained. */
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
    OresNativeFailureTraceReport* out);

/* Render into caller-owned storage. NULL + zero capacity computes required_bytes.
 * required_bytes excludes the NUL; nonzero capacity is always NUL-terminated. */
int ores_native_failure_trace_render(
    OresNativeFailureTraceReport* report,
    const OresNativeExecutionTraceFrame* execution_frames,
    const OresNativeContinuationTraceFrame* continuation_frames,
    char* buffer,
    size_t capacity);

const char* ores_native_failure_trace_kind_name(OresNativeFailureTraceKind kind);

#ifdef __cplusplus
}
#endif
