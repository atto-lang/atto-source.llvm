#pragma once

#include "oreslang/runtime/NativeLogicalTrace.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ORES_NATIVE_FAILURE_THROW = 1,
    ORES_NATIVE_FAILURE_RAISE = 2,
    ORES_NATIVE_FAILURE_PANIC = 3
} OresNativeFailureKind;

/*
 * Snapshot header captured synchronously at the failure site. It contains
 * values/handles only; native frames, if available, are symbolized/rendered
 * separately while the active native failure stack is still alive.
 */
typedef struct {
    OresNativeFailureKind kind;
    uint32_t reserved;
    uint64_t source_token;
    OresNativeExecutionHandle current_execution;
    OresNativeExecutionId current_execution_id;
    OresNativeAllocationDomainRef allocation;
    OresNativeLogicalTraceResult logical;
} OresNativeFailureTraceHeader;

/*
 * Capture the logical portion of a throw/raise/panic trace. The current
 * execution is retained for the duration of the snapshot so parent execution
 * ancestry cannot disappear midway through the walk.
 */
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
    OresNativeFailureTraceHeader* out);

/*
 * Allocation-free logical renderer. It is intentionally separate from native
 * symbolization: callers can render live native frames first and append this
 * continuation/execution ancestry afterward. Returns bytes written excluding
 * the trailing NUL; output is always NUL-terminated when capacity != 0.
 */
size_t ores_native_failure_trace_format(
    const OresNativeFailureTraceHeader* header,
    const OresNativeExecutionTraceFrame* execution_frames,
    const OresNativeContinuationTraceFrame* continuation_frames,
    char* buffer,
    size_t capacity);

#ifdef __cplusplus
}
#endif
