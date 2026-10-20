#pragma once
#include <stddef.h>
#include <stdint.h>
#include "oreslang/runtime/NativeExecutionContext.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Generation checked logical identity, never a stack or heap pointer. */
typedef struct {
    uint64_t store_id;
    uint64_t slot_generation;
} OresNativeTraceHandle;

/* Archival/snapshot trace record only. This is deliberately distinct from
 * OresNativeContinuationRecord in NativeContinuationStore.h, which owns live
 * resume state and execution pins. No native frame pointers are retained. */
typedef struct {
    OresNativeTraceHandle parent_execution;
    OresNativeTraceHandle parent_continuation;
    uint64_t source_span;
    uint64_t await_site;
    uint64_t causal_kind;
    uint64_t reserved;
} OresNativeTraceContinuationRecord;

/* Opaque, preallocated, bounded AoSoA with 8 lanes per block. */
typedef struct OresNativeTraceStore OresNativeTraceStore;

OresNativeTraceStore* ores_native_execution_store_open(size_t capacity);
OresNativeTraceStore* ores_native_continuation_store_open(size_t capacity);
void ores_native_trace_store_close(OresNativeTraceStore* store);

/* A zero handle is invalid; stores fail closed on exhaustion. */
OresNativeTraceHandle ores_native_execution_store_insert(
    OresNativeTraceStore* store, const OresNativeExecutionContext* value);
OresNativeTraceHandle ores_native_continuation_store_insert(
    OresNativeTraceStore* store, const OresNativeTraceContinuationRecord* value);
int ores_native_execution_store_load(OresNativeTraceStore* store,
    OresNativeTraceHandle handle, OresNativeExecutionContext* out);
int ores_native_continuation_store_load(OresNativeTraceStore* store,
    OresNativeTraceHandle handle, OresNativeTraceContinuationRecord* out);
int ores_native_trace_store_release(OresNativeTraceStore* store,
    OresNativeTraceHandle handle);

#ifdef __cplusplus
}
static_assert(sizeof(OresNativeTraceContinuationRecord) == 64,
              "continuation record ABI must be exactly 64 bytes");
#else
_Static_assert(sizeof(OresNativeTraceContinuationRecord) == 64,
               "continuation record ABI must be exactly 64 bytes");
#endif
