#pragma once

#include "oreslang/runtime/NativeExecutionContext.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ORES_NATIVE_AOSOA_LANES 8u

typedef struct OresNativeExecutionContextStore OresNativeExecutionContextStore;

/*
 * Fixed-capacity, generation-checked AoSoA storage for logical executions.
 * Store records contain values and handles only; no native stack pointers.
 */
OresNativeExecutionContextStore* ores_native_execution_context_store_open(
    uint32_t capacity);

/*
 * Close succeeds only after every logical reference has been released.
 * Callers must externally quiesce producers before closing.
 */
int ores_native_execution_context_store_close(
    OresNativeExecutionContextStore* store);

int ores_native_execution_context_store_insert(
    OresNativeExecutionContextStore* store,
    const OresNativeExecutionContext* context,
    OresNativeExecutionHandle* out);

int ores_native_execution_context_store_get(
    OresNativeExecutionContextStore* store,
    OresNativeExecutionHandle handle,
    OresNativeExecutionContext* out);

int ores_native_execution_context_store_retain(
    OresNativeExecutionContextStore* store,
    OresNativeExecutionHandle handle);

int ores_native_execution_context_store_release(
    OresNativeExecutionContextStore* store,
    OresNativeExecutionHandle handle);

/*
 * One-shot weak link from an execution record to the continuation created at
 * its suspension boundary. The ContinuationStore owns/pins continuation life.
 */
int ores_native_execution_context_store_bind_continuation(
    OresNativeExecutionContextStore* store,
    OresNativeExecutionHandle execution,
    OresNativeContinuationHandle continuation);

uint32_t ores_native_execution_context_store_capacity(
    OresNativeExecutionContextStore* store);
uint32_t ores_native_execution_context_store_live(
    OresNativeExecutionContextStore* store);

#ifdef __cplusplus
}
#endif
