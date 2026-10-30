#pragma once

#include "oreslang/runtime/NativeContinuationStore.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ORES_CALLBACK_BOUNDARY_HOST_CALLBACK = 1,
    ORES_CALLBACK_BOUNDARY_IO = 2,
    ORES_CALLBACK_BOUNDARY_TIMER = 3,
    ORES_CALLBACK_BOUNDARY_AWAIT = 4,
    ORES_CALLBACK_BOUNDARY_ACTOR_MESSAGE = 5,
    ORES_CALLBACK_BOUNDARY_SUPERVISOR_CONTROL = 6,
    ORES_CALLBACK_BOUNDARY_TASK_JOIN = 7,
    ORES_CALLBACK_BOUNDARY_SPAWN = 8
} OresNativeCallbackBoundary;

typedef struct {
    OresNativeExecutionId source_execution_id;
    OresNativeExecutionHandle source_execution;
    OresNativeContinuationHandle source_continuation;
    uint32_t boundary;
    uint32_t flags;
} OresNativeCallbackContext;

typedef struct {
    OresNativeExecutionId source_execution_id;
    OresNativeExecutionHandle source_execution;
    OresNativeContinuationHandle source_continuation;
    OresNativeCallbackBoundary boundary;
} OresNativeCallbackProvenance;

/*
 * Captures one retained callback-provenance token.
 *
 * out must point to an all-zero OresNativeCallbackContext. Capture fails rather
 * than overwriting a live token, because overwriting would leak the retained
 * execution/continuation lineage. Release is idempotent only for the canonical
 * all-zero state produced by initialization or a successful release.
 */
int ores_native_callback_context_capture(
    OresNativeExecutionContextStore* executions,
    OresNativeContinuationStore* continuations,
    OresNativeExecutionHandle source_execution,
    OresNativeContinuationHandle source_continuation,
    OresNativeCallbackBoundary boundary,
    OresNativeCallbackContext* out);

int ores_native_callback_context_snapshot(
    OresNativeExecutionContextStore* executions,
    OresNativeContinuationStore* continuations,
    const OresNativeCallbackContext* context,
    OresNativeCallbackProvenance* out);

int ores_native_callback_context_release(
    OresNativeExecutionContextStore* executions,
    OresNativeContinuationStore* continuations,
    OresNativeCallbackContext* context);

#ifdef __cplusplus
}
static_assert(sizeof(OresNativeCallbackContext) == 40,
              "callback context ABI must remain fixed-width and pointer-free");
#else
_Static_assert(sizeof(OresNativeCallbackContext) == 40,
               "callback context ABI must remain fixed-width and pointer-free");
#endif
