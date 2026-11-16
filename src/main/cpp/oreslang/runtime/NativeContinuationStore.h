#pragma once

#include "oreslang/runtime/NativeExecutionContextStore.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ORES_CONTINUATION_BOUNDARY_AWAIT = 1,
    ORES_CONTINUATION_BOUNDARY_SPAWN = 2,
    ORES_CONTINUATION_BOUNDARY_ACTOR_MESSAGE = 3,
    ORES_CONTINUATION_BOUNDARY_SUPERVISOR_CONTROL = 4,
    ORES_CONTINUATION_BOUNDARY_TASK_JOIN = 5
} OresNativeContinuationBoundary;

typedef enum {
    ORES_CONTINUATION_PENDING = 1,
    ORES_CONTINUATION_CLAIMED = 2,
    ORES_CONTINUATION_COMPLETED = 3,
    ORES_CONTINUATION_CANCELLED = 4,
    ORES_CONTINUATION_READY = 5
} OresNativeContinuationState;

typedef struct {
    OresNativeExecutionHandle execution;
    OresNativeExecutionId execution_id;
    OresNativeContinuationHandle parent;
    OresNativeAllocationDomainRef allocation;
    uint64_t code_generation;
    uint64_t source_token;
    OresNativeContinuationBoundary boundary;
    OresNativeContinuationState state;
} OresNativeContinuationRecord;

typedef struct OresNativeContinuationStore OresNativeContinuationStore;

/*
 * Runs under the continuation-store lock. The callback must be bounded and
 * must not wait for work that can depend on continuation stores. Any
 * continuation-store API re-entry fails closed rather than risking a
 * same-store self-deadlock or cross-store lock-order cycle.
 */
typedef int (*OresNativeContinuationReadyCommit)(void* context);

/*
 * The continuation store pins each originating execution and parent
 * continuation by logical reference. Records are pointer-free AoSoA lanes.
 */
OresNativeContinuationStore* ores_native_continuation_store_open(
    uint32_t capacity,
    OresNativeExecutionContextStore* executions);

/* Close requires every API user/producer to be externally quiescent and all
 * logical references released; live_count alone is not a shutdown fence.
 * The execution store must outlive this continuation store. */
int ores_native_continuation_store_close(OresNativeContinuationStore* store);

int ores_native_continuation_store_publish(
    OresNativeContinuationStore* store,
    OresNativeExecutionHandle execution,
    OresNativeContinuationHandle parent,
    OresNativeContinuationBoundary boundary,
    uint64_t code_generation,
    uint64_t source_token,
    OresNativeContinuationHandle* out);

int ores_native_continuation_store_get(
    OresNativeContinuationStore* store,
    OresNativeContinuationHandle handle,
    OresNativeContinuationRecord* out);

int ores_native_continuation_store_retain(
    OresNativeContinuationStore* store,
    OresNativeContinuationHandle handle);

/* The final reference cannot retire a CLAIMED frame. Its owner must finish
 * execution first; a rejected release leaves the reference unchanged. */
int ores_native_continuation_store_release(
    OresNativeContinuationStore* store,
    OresNativeContinuationHandle handle);

/*
 * Future/channel readiness path: publish completion before any scheduler
 * resume claim. A READY continuation may still lose to cancellation until
 * the scheduler claims it. Both operations serialize against the same store
 * lock. These are host primitives, not source-level await/select lowering.
 */
/*
 * Atomically relative to cancellation: require PENDING, run the fallible
 * producer commit while holding the store lock, then publish READY with a
 * non-failing in-memory state transition. If commit returns false, the
 * continuation remains PENDING. Continuation-store callback re-entry is rejected.
 */
int ores_native_continuation_store_commit_ready(
    OresNativeContinuationStore* store,
    OresNativeContinuationHandle handle,
    OresNativeContinuationReadyCommit commit,
    void* context);
int ores_native_continuation_store_notify_ready(
    OresNativeContinuationStore* store,
    OresNativeContinuationHandle handle);
int ores_native_continuation_store_try_claim_ready_resume(
    OresNativeContinuationStore* store,
    OresNativeContinuationHandle handle,
    OresNativeContinuationRecord* out);

/* Legacy direct trace-resume path; NOT valid for an unresolved Future. */
/* Exactly one resume owner may claim a pending continuation. */
int ores_native_continuation_store_try_claim_resume(
    OresNativeContinuationStore* store,
    OresNativeContinuationHandle handle,
    OresNativeContinuationRecord* out);

int ores_native_continuation_store_finish_resume(
    OresNativeContinuationStore* store,
    OresNativeContinuationHandle handle);

int ores_native_continuation_store_cancel(
    OresNativeContinuationStore* store,
    OresNativeContinuationHandle handle);

uint32_t ores_native_continuation_store_capacity(
    OresNativeContinuationStore* store);
uint32_t ores_native_continuation_store_live(
    OresNativeContinuationStore* store);

#ifdef __cplusplus
}
#endif
