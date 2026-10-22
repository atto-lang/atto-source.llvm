#pragma once

#include "oreslang/runtime/NativeAwaitTraceBridge.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    OresNativeExecutionHandle handle;
    OresNativeExecutionContext context;
} OresNativeExecutionTraceFrame;

typedef struct {
    OresNativeContinuationHandle handle;
    OresNativeContinuationRecord record;
} OresNativeContinuationTraceFrame;

typedef struct {
    size_t execution_count;
    size_t continuation_count;
    int execution_truncated;
    int continuation_truncated;
    int corrupt;
} OresNativeLogicalTraceResult;

/*
 * Snapshot logical ancestry into caller-owned buffers. This function allocates
 * no memory and stores no native frame address. Native symbolized frames from
 * the active failure stack can be rendered first, then these logical segments,
 * then actor/supervisor boundary records carried by continuation.boundary.
 */
int ores_native_logical_trace_snapshot(
    OresNativeExecutionContextStore* executions,
    OresNativeContinuationStore* continuations,
    OresNativeExecutionHandle current_execution,
    OresNativeExecutionTraceFrame* execution_frames,
    size_t execution_capacity,
    OresNativeContinuationTraceFrame* continuation_frames,
    size_t continuation_capacity,
    OresNativeLogicalTraceResult* out);

#ifdef __cplusplus
}
#endif
