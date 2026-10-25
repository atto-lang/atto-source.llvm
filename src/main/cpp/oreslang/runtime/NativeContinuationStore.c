#include "oreslang/runtime/NativeContinuationStore.h"
#include "oreslang/runtime/NativeTraceSlotNamespace.h"

#include <limits.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>

typedef struct {
    uint32_t generation[ORES_NATIVE_AOSOA_LANES];
    uint32_t refs[ORES_NATIVE_AOSOA_LANES];
    unsigned char live[ORES_NATIVE_AOSOA_LANES];
    unsigned char state[ORES_NATIVE_AOSOA_LANES];
    unsigned char boundary[ORES_NATIVE_AOSOA_LANES];

    uint32_t execution_slot[ORES_NATIVE_AOSOA_LANES];
    uint32_t execution_generation[ORES_NATIVE_AOSOA_LANES];
    uint64_t execution_runtime_domain[ORES_NATIVE_AOSOA_LANES];
    uint64_t execution_id[ORES_NATIVE_AOSOA_LANES];

    uint32_t parent_slot[ORES_NATIVE_AOSOA_LANES];
    uint32_t parent_generation[ORES_NATIVE_AOSOA_LANES];

    uint64_t allocation_kind[ORES_NATIVE_AOSOA_LANES];
    uint64_t allocation_domain[ORES_NATIVE_AOSOA_LANES];
    uint64_t allocation_owner[ORES_NATIVE_AOSOA_LANES];
    uint64_t allocation_generation[ORES_NATIVE_AOSOA_LANES];

    uint64_t code_generation[ORES_NATIVE_AOSOA_LANES];
    uint64_t source_token[ORES_NATIVE_AOSOA_LANES];
} ContinuationBlock;

struct OresNativeContinuationStore {
    pthread_mutex_t lock;
    ContinuationBlock* blocks;
    OresNativeExecutionContextStore* executions;
    uint32_t capacity;
    uint32_t first_slot;
    uint32_t block_count;
    uint32_t live_count;
    uint32_t cursor;
};

/*
 * A producer commit executes while one continuation store is locked. Any
 * continuation-store API re-entry from that callback fails closed. Allowing
 * cross-store re-entry would permit A->B / B->A lock-order cycles.
 */
static _Thread_local unsigned ready_commit_depth;

static int lock_store(OresNativeContinuationStore* store) {
    if (ready_commit_depth != 0) return 0;
    return pthread_mutex_lock(&store->lock) == 0;
}
static void unlock_store(OresNativeContinuationStore* store) {
    if (pthread_mutex_unlock(&store->lock) != 0) abort();
}

static ContinuationBlock* block_for(
        OresNativeContinuationStore* store, uint32_t index, uint32_t* lane) {
    *lane = index % ORES_NATIVE_AOSOA_LANES;
    return &store->blocks[index / ORES_NATIVE_AOSOA_LANES];
}

static int boundary_valid(OresNativeContinuationBoundary boundary) {
    return boundary >= ORES_CONTINUATION_BOUNDARY_AWAIT
        && boundary <= ORES_CONTINUATION_BOUNDARY_TASK_JOIN;
}

static int resolve_locked(
        OresNativeContinuationStore* store,
        OresNativeContinuationHandle handle,
        ContinuationBlock** block,
        uint32_t* lane,
        uint32_t* index) {
    if (!ores_native_store_handle_valid(handle)
            || handle.slot < store->first_slot
            || (uint64_t)(handle.slot - store->first_slot) >= store->capacity)
        return 0;
    *index = handle.slot - store->first_slot;
    *block = block_for(store, *index, lane);
    return (*block)->live[*lane]
        && (*block)->generation[*lane] == handle.generation;
}

static OresNativeContinuationRecord load_record(
        const ContinuationBlock* block, uint32_t lane) {
    OresNativeContinuationRecord record;
    record.execution.slot = block->execution_slot[lane];
    record.execution.generation = block->execution_generation[lane];
    record.execution_id.runtime_domain = block->execution_runtime_domain[lane];
    record.execution_id.execution = block->execution_id[lane];
    record.parent.slot = block->parent_slot[lane];
    record.parent.generation = block->parent_generation[lane];
    record.allocation.kind = block->allocation_kind[lane];
    record.allocation.domain = block->allocation_domain[lane];
    record.allocation.owner = block->allocation_owner[lane];
    record.allocation.generation = block->allocation_generation[lane];
    record.code_generation = block->code_generation[lane];
    record.source_token = block->source_token[lane];
    record.boundary =
        (OresNativeContinuationBoundary)block->boundary[lane];
    record.state = (OresNativeContinuationState)block->state[lane];
    return record;
}

OresNativeContinuationStore* ores_native_continuation_store_open(
        uint32_t capacity,
        OresNativeExecutionContextStore* executions) {
    if (capacity == 0 || !executions) return NULL;
    const uint64_t blocks64 =
        ((uint64_t)capacity + ORES_NATIVE_AOSOA_LANES - 1u)
        / ORES_NATIVE_AOSOA_LANES;
    if (blocks64 > UINT32_MAX
            || blocks64 > SIZE_MAX / sizeof(ContinuationBlock))
        return NULL;

    OresNativeContinuationStore* store =
        (OresNativeContinuationStore*)calloc(1, sizeof(*store));
    if (!store) return NULL;
    store->blocks = (ContinuationBlock*)calloc(
        (size_t)blocks64, sizeof(ContinuationBlock));
    if (!store->blocks) {
        free(store);
        return NULL;
    }
    if (pthread_mutex_init(&store->lock, NULL) != 0) {
        free(store->blocks);
        free(store);
        return NULL;
    }
    if (!ores_native_trace_reserve_slot_namespace(capacity, &store->first_slot)) {
        pthread_mutex_destroy(&store->lock);
        free(store->blocks);
        free(store);
        return NULL;
    }
    store->capacity = capacity;
    store->block_count = (uint32_t)blocks64;
    store->executions = executions;
    for (uint32_t index = 0; index < capacity; ++index) {
        uint32_t lane = 0;
        ContinuationBlock* block = block_for(store, index, &lane);
        block->generation[lane] = 1;
    }
    return store;
}

int ores_native_continuation_store_close(OresNativeContinuationStore* store) {
    if (!store || !lock_store(store)) return 0;
    const int empty = store->live_count == 0;
    unlock_store(store);
    if (!empty) return 0;
    if (pthread_mutex_destroy(&store->lock) != 0) return 0;
    free(store->blocks);
    free(store);
    return 1;
}

int ores_native_continuation_store_publish(
        OresNativeContinuationStore* store,
        OresNativeExecutionHandle execution,
        OresNativeContinuationHandle parent,
        OresNativeContinuationBoundary boundary,
        uint64_t code_generation,
        uint64_t source_token,
        OresNativeContinuationHandle* out) {
    if (!store || !out || !ores_native_store_handle_valid(execution)
            || ((parent.slot == 0) != (parent.generation == 0))
            || !boundary_valid(boundary))
        return 0;

    OresNativeExecutionContext context;
    if (!ores_native_execution_context_store_get(
            store->executions, execution, &context)
            || !ores_native_execution_context_store_retain(
                store->executions, execution))
        return 0;

    if (!lock_store(store)) {
        (void)ores_native_execution_context_store_release(
            store->executions, execution);
        return 0;
    }

    uint32_t parent_lane = 0, parent_index = 0;
    ContinuationBlock* parent_block = NULL;
    if (ores_native_store_handle_valid(parent)) {
        if (!resolve_locked(store, parent,
                &parent_block, &parent_lane, &parent_index)
                || parent_block->execution_runtime_domain[parent_lane]
                    != context.id.runtime_domain
                || (parent_block->state[parent_lane] != ORES_CONTINUATION_CLAIMED
                    && parent_block->state[parent_lane] != ORES_CONTINUATION_COMPLETED)
                || parent_block->refs[parent_lane] == UINT32_MAX) {
            unlock_store(store);
            (void)ores_native_execution_context_store_release(
                store->executions, execution);
            return 0;
        }
    }

    uint32_t free_index = UINT32_MAX;
    for (uint32_t offset = 0; offset < store->capacity; ++offset) {
        const uint32_t index =
            (uint32_t)(((uint64_t)store->cursor + offset) % store->capacity);
        uint32_t lane = 0;
        ContinuationBlock* block = block_for(store, index, &lane);
        if (!block->live[lane] && block->generation[lane] != 0) {
            free_index = index;
            break;
        }
    }
    if (free_index == UINT32_MAX) {
        unlock_store(store);
        (void)ores_native_execution_context_store_release(
            store->executions, execution);
        return 0;
    }

    if (parent_block) ++parent_block->refs[parent_lane];

    uint32_t lane = 0;
    ContinuationBlock* block = block_for(store, free_index, &lane);
    block->execution_slot[lane] = execution.slot;
    block->execution_generation[lane] = execution.generation;
    block->execution_runtime_domain[lane] = context.id.runtime_domain;
    block->execution_id[lane] = context.id.execution;
    block->parent_slot[lane] = parent.slot;
    block->parent_generation[lane] = parent.generation;
    block->allocation_kind[lane] = context.allocation.kind;
    block->allocation_domain[lane] = context.allocation.domain;
    block->allocation_owner[lane] = context.allocation.owner;
    block->allocation_generation[lane] = context.allocation.generation;
    block->code_generation[lane] = code_generation;
    block->source_token[lane] = source_token;
    block->boundary[lane] = (unsigned char)boundary;
    block->state[lane] = (unsigned char)ORES_CONTINUATION_PENDING;
    block->refs[lane] = 1;
    block->live[lane] = 1;
    ++store->live_count;
    store->cursor = free_index + 1u == store->capacity ? 0 : free_index + 1u;

    out->slot = store->first_slot + free_index;
    out->generation = block->generation[lane];
    unlock_store(store);
    return 1;
}

int ores_native_continuation_store_get(
        OresNativeContinuationStore* store,
        OresNativeContinuationHandle handle,
        OresNativeContinuationRecord* out) {
    if (!store || !out || !lock_store(store)) return 0;
    uint32_t lane = 0, index = 0;
    ContinuationBlock* block = NULL;
    const int ok = resolve_locked(store, handle, &block, &lane, &index);
    if (ok) *out = load_record(block, lane);
    unlock_store(store);
    return ok;
}

int ores_native_continuation_store_retain(
        OresNativeContinuationStore* store,
        OresNativeContinuationHandle handle) {
    if (!store || !lock_store(store)) return 0;
    uint32_t lane = 0, index = 0;
    ContinuationBlock* block = NULL;
    int ok = resolve_locked(store, handle, &block, &lane, &index)
        && block->refs[lane] != UINT32_MAX;
    if (ok) ++block->refs[lane];
    unlock_store(store);
    return ok;
}

int ores_native_continuation_store_release(
        OresNativeContinuationStore* store,
        OresNativeContinuationHandle handle) {
    if (!store) return 0;
    OresNativeContinuationHandle current = handle;
    int released_any = 0;

    while (ores_native_store_handle_valid(current)) {
        if (!lock_store(store)) return 0;
        uint32_t lane = 0, index = 0;
        ContinuationBlock* block = NULL;
        if (!resolve_locked(store, current, &block, &lane, &index)
                || block->refs[lane] == 0) {
            unlock_store(store);
            return 0;
        }
        if (block->refs[lane] > 1) {
            --block->refs[lane];
            released_any = 1;
            unlock_store(store);
            break;
        }

        /* A claimed frame still owns physical execution. Only its claimant
         * may finish it; dropping the final reference must not retire it. */
        if (block->state[lane] == ORES_CONTINUATION_CLAIMED) {
            unlock_store(store);
            return released_any;
        }

        released_any = 1;
        const OresNativeExecutionHandle execution = {
            block->execution_slot[lane],
            block->execution_generation[lane]
        };
        const OresNativeContinuationHandle parent = {
            block->parent_slot[lane],
            block->parent_generation[lane]
        };

        block->refs[lane] = 0;
        block->live[lane] = 0;
        block->state[lane] = 0;
        block->boundary[lane] = 0;
        block->execution_slot[lane] = 0;
        block->execution_generation[lane] = 0;
        block->execution_runtime_domain[lane] = 0;
        block->execution_id[lane] = 0;
        block->parent_slot[lane] = 0;
        block->parent_generation[lane] = 0;
        block->allocation_kind[lane] = 0;
        block->allocation_domain[lane] = 0;
        block->allocation_owner[lane] = 0;
        block->allocation_generation[lane] = 0;
        block->code_generation[lane] = 0;
        block->source_token[lane] = 0;
        if (block->generation[lane] == UINT32_MAX)
            block->generation[lane] = 0; /* permanently retired */
        else
            ++block->generation[lane];
        if (store->live_count == 0) abort();
        --store->live_count;
        store->cursor = index;
        unlock_store(store);

        if (!ores_native_execution_context_store_release(
                store->executions, execution))
            return 0;
        current = parent;
    }
    return released_any;
}

int ores_native_continuation_store_commit_ready(
        OresNativeContinuationStore* store,
        OresNativeContinuationHandle handle,
        OresNativeContinuationReadyCommit commit,
        void* context) {
    if (!store || !commit || !lock_store(store)) return 0;
    uint32_t lane = 0, index = 0;
    ContinuationBlock* block = NULL;
    int ok = resolve_locked(store, handle, &block, &lane, &index)
        && block->state[lane] == ORES_CONTINUATION_PENDING;
    if (ok) {
        ++ready_commit_depth;
        ok = commit(context);
        --ready_commit_depth;
        if (ok)
            block->state[lane] = (unsigned char)ORES_CONTINUATION_READY;
    }
    unlock_store(store);
    return ok;
}

int ores_native_continuation_store_notify_ready(
        OresNativeContinuationStore* store,
        OresNativeContinuationHandle handle) {
    if (!store || !lock_store(store)) return 0;
    uint32_t lane = 0, index = 0;
    ContinuationBlock* block = NULL;
    int ok = resolve_locked(store, handle, &block, &lane, &index)
        && block->state[lane] == ORES_CONTINUATION_PENDING;
    if (ok) block->state[lane] = (unsigned char)ORES_CONTINUATION_READY;
    unlock_store(store);
    return ok;
}

int ores_native_continuation_store_try_claim_ready_resume(
        OresNativeContinuationStore* store,
        OresNativeContinuationHandle handle,
        OresNativeContinuationRecord* out) {
    if (!store || !lock_store(store)) return 0;
    uint32_t lane = 0, index = 0;
    ContinuationBlock* block = NULL;
    int ok = resolve_locked(store, handle, &block, &lane, &index)
        && block->state[lane] == ORES_CONTINUATION_READY;
    if (ok) {
        block->state[lane] = (unsigned char)ORES_CONTINUATION_CLAIMED;
        if (out) *out = load_record(block, lane);
    }
    unlock_store(store);
    return ok;
}

int ores_native_continuation_store_try_claim_resume(
        OresNativeContinuationStore* store,
        OresNativeContinuationHandle handle,
        OresNativeContinuationRecord* out) {
    if (!store || !lock_store(store)) return 0;
    uint32_t lane = 0, index = 0;
    ContinuationBlock* block = NULL;
    int ok = resolve_locked(store, handle, &block, &lane, &index)
        && block->state[lane] == ORES_CONTINUATION_PENDING;
    if (ok) {
        block->state[lane] = (unsigned char)ORES_CONTINUATION_CLAIMED;
        if (out) *out = load_record(block, lane);
    }
    unlock_store(store);
    return ok;
}

int ores_native_continuation_store_finish_resume(
        OresNativeContinuationStore* store,
        OresNativeContinuationHandle handle) {
    if (!store || !lock_store(store)) return 0;
    uint32_t lane = 0, index = 0;
    ContinuationBlock* block = NULL;
    int ok = resolve_locked(store, handle, &block, &lane, &index)
        && block->state[lane] == ORES_CONTINUATION_CLAIMED;
    if (ok) block->state[lane] = (unsigned char)ORES_CONTINUATION_COMPLETED;
    unlock_store(store);
    return ok;
}

int ores_native_continuation_store_cancel(
        OresNativeContinuationStore* store,
        OresNativeContinuationHandle handle) {
    if (!store || !lock_store(store)) return 0;
    uint32_t lane = 0, index = 0;
    ContinuationBlock* block = NULL;
    int ok = resolve_locked(store, handle, &block, &lane, &index)
        && (block->state[lane] == ORES_CONTINUATION_PENDING
            || block->state[lane] == ORES_CONTINUATION_READY);
    if (ok) block->state[lane] = (unsigned char)ORES_CONTINUATION_CANCELLED;
    unlock_store(store);
    return ok;
}

uint32_t ores_native_continuation_store_capacity(
        OresNativeContinuationStore* store) {
    if (!store) return 0;
    return store->capacity;
}

uint32_t ores_native_continuation_store_live(
        OresNativeContinuationStore* store) {
    if (!store || !lock_store(store)) return 0;
    const uint32_t live = store->live_count;
    unlock_store(store);
    return live;
}
