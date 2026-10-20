#include "oreslang/runtime/NativeExecutionContextStore.h"
#include "oreslang/runtime/NativeTraceSlotNamespace.h"

#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint32_t generation[ORES_NATIVE_AOSOA_LANES];
    uint32_t refs[ORES_NATIVE_AOSOA_LANES];
    unsigned char live[ORES_NATIVE_AOSOA_LANES];

    uint64_t runtime_domain[ORES_NATIVE_AOSOA_LANES];
    uint64_t execution[ORES_NATIVE_AOSOA_LANES];

    uint64_t allocation_kind[ORES_NATIVE_AOSOA_LANES];
    uint64_t allocation_domain[ORES_NATIVE_AOSOA_LANES];
    uint64_t allocation_owner[ORES_NATIVE_AOSOA_LANES];
    uint64_t allocation_generation[ORES_NATIVE_AOSOA_LANES];

    uint32_t parent_slot[ORES_NATIVE_AOSOA_LANES];
    uint32_t parent_generation[ORES_NATIVE_AOSOA_LANES];
    uint32_t continuation_slot[ORES_NATIVE_AOSOA_LANES];
    uint32_t continuation_generation[ORES_NATIVE_AOSOA_LANES];
} ExecutionBlock;

struct OresNativeExecutionContextStore {
    pthread_mutex_t lock;
    ExecutionBlock* blocks;
    uint32_t capacity;
    uint32_t first_slot;
    uint32_t block_count;
    uint32_t live_count;
    uint32_t cursor;
};

/*
 * All execution and continuation stores share this monotonic range allocator.
 * A public handle's slot identifies one store instance for the lifetime of
 * the process; slot ranges are not recycled after close. Keep all 32 bits of
 * the generation for per-slot ABA protection.
 */
static _Atomic(uint64_t) next_trace_slot = 1;

int ores_native_trace_reserve_slot_namespace(
        uint32_t capacity, uint32_t* first_slot) {
    if (capacity == 0 || !first_slot) return 0;
    uint64_t current = atomic_load(&next_trace_slot);
    for (;;) {
        if (current == 0 ||
                current > UINT64_C(4294967296) - capacity)
            return 0;
        if (atomic_compare_exchange_weak(
                &next_trace_slot, &current, current + capacity)) {
            *first_slot = (uint32_t)current;
            return 1;
        }
    }
}

static int lock_store(OresNativeExecutionContextStore* store) {
    return pthread_mutex_lock(&store->lock) == 0;
}
static void unlock_store(OresNativeExecutionContextStore* store) {
    if (pthread_mutex_unlock(&store->lock) != 0) abort();
}

static ExecutionBlock* block_for(
        OresNativeExecutionContextStore* store, uint32_t index, uint32_t* lane) {
    *lane = index % ORES_NATIVE_AOSOA_LANES;
    return &store->blocks[index / ORES_NATIVE_AOSOA_LANES];
}

static int same_handle(OresNativeStoreHandle a, OresNativeStoreHandle b) {
    return a.slot == b.slot && a.generation == b.generation;
}

static int allocation_valid(OresNativeAllocationDomainRef allocation) {
    if (allocation.kind > ORES_ALLOCATION_DOMAIN_RUNTIME) return 0;
    if (allocation.kind == ORES_ALLOCATION_DOMAIN_NONE)
        return allocation.domain == 0 && allocation.owner == 0
            && allocation.generation == 0;
    /*
     * owner==0 is valid (actor slot zero is a real capability). Domain and
     * generation provide the non-zero authority fence.
     */
    return allocation.domain != 0 && allocation.generation != 0;
}

static int handle_shape_valid_or_empty(OresNativeStoreHandle handle) {
    return (handle.slot == 0 && handle.generation == 0)
        || ores_native_store_handle_valid(handle);
}

static int resolve_locked(
        OresNativeExecutionContextStore* store,
        OresNativeExecutionHandle handle,
        ExecutionBlock** block,
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

static OresNativeExecutionContext load_context(
        const ExecutionBlock* block, uint32_t lane) {
    OresNativeExecutionContext context;
    context.id.runtime_domain = block->runtime_domain[lane];
    context.id.execution = block->execution[lane];
    context.allocation.kind = block->allocation_kind[lane];
    context.allocation.domain = block->allocation_domain[lane];
    context.allocation.owner = block->allocation_owner[lane];
    context.allocation.generation = block->allocation_generation[lane];
    context.parent_execution.slot = block->parent_slot[lane];
    context.parent_execution.generation = block->parent_generation[lane];
    context.continuation.slot = block->continuation_slot[lane];
    context.continuation.generation = block->continuation_generation[lane];
    return context;
}

OresNativeExecutionContextStore* ores_native_execution_context_store_open(
        uint32_t capacity) {
    if (capacity == 0) return NULL;
    const uint64_t blocks64 =
        ((uint64_t)capacity + ORES_NATIVE_AOSOA_LANES - 1u)
        / ORES_NATIVE_AOSOA_LANES;
    if (blocks64 > UINT32_MAX
            || blocks64 > SIZE_MAX / sizeof(ExecutionBlock))
        return NULL;

    OresNativeExecutionContextStore* store =
        (OresNativeExecutionContextStore*)calloc(1, sizeof(*store));
    if (!store) return NULL;
    store->blocks = (ExecutionBlock*)calloc(
        (size_t)blocks64, sizeof(ExecutionBlock));
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

    for (uint32_t index = 0; index < capacity; ++index) {
        uint32_t lane = 0;
        ExecutionBlock* block = block_for(store, index, &lane);
        block->generation[lane] = 1;
    }
    return store;
}

int ores_native_execution_context_store_close(
        OresNativeExecutionContextStore* store) {
    if (!store || !lock_store(store)) return 0;
    const int empty = store->live_count == 0;
    unlock_store(store);
    if (!empty) return 0;
    if (pthread_mutex_destroy(&store->lock) != 0) return 0;
    free(store->blocks);
    free(store);
    return 1;
}

int ores_native_execution_context_store_insert(
        OresNativeExecutionContextStore* store,
        const OresNativeExecutionContext* context,
        OresNativeExecutionHandle* out) {
    if (!store || !context || !out
            || context->id.runtime_domain == 0
            || context->id.execution == 0
            || !allocation_valid(context->allocation)
            || !handle_shape_valid_or_empty(context->parent_execution)
            || !handle_shape_valid_or_empty(context->continuation)
            || !lock_store(store))
        return 0;

    uint32_t parent_index = 0, parent_lane = 0;
    ExecutionBlock* parent_block = NULL;
    if (ores_native_store_handle_valid(context->parent_execution)) {
        if (!resolve_locked(store, context->parent_execution,
                &parent_block, &parent_lane, &parent_index)
                || parent_block->runtime_domain[parent_lane]
                    != context->id.runtime_domain
                || parent_block->refs[parent_lane] == UINT32_MAX) {
            unlock_store(store);
            return 0;
        }
    }

    uint32_t free_index = UINT32_MAX;
    for (uint32_t offset = 0; offset < store->capacity; ++offset) {
        const uint32_t index =
            (uint32_t)(((uint64_t)store->cursor + offset) % store->capacity);
        uint32_t lane = 0;
        ExecutionBlock* block = block_for(store, index, &lane);
        if (block->live[lane]) {
            if (block->runtime_domain[lane] == context->id.runtime_domain
                    && block->execution[lane] == context->id.execution) {
                unlock_store(store);
                return 0; /* duplicate logical identity */
            }
        } else if (block->generation[lane] != 0
                && free_index == UINT32_MAX) {
            free_index = index;
        }
    }
    if (free_index == UINT32_MAX) {
        unlock_store(store);
        return 0;
    }

    if (parent_block) ++parent_block->refs[parent_lane];

    uint32_t lane = 0;
    ExecutionBlock* block = block_for(store, free_index, &lane);
    block->runtime_domain[lane] = context->id.runtime_domain;
    block->execution[lane] = context->id.execution;
    block->allocation_kind[lane] = context->allocation.kind;
    block->allocation_domain[lane] = context->allocation.domain;
    block->allocation_owner[lane] = context->allocation.owner;
    block->allocation_generation[lane] = context->allocation.generation;
    block->parent_slot[lane] = context->parent_execution.slot;
    block->parent_generation[lane] = context->parent_execution.generation;
    block->continuation_slot[lane] = context->continuation.slot;
    block->continuation_generation[lane] = context->continuation.generation;
    block->refs[lane] = 1;
    block->live[lane] = 1;
    ++store->live_count;
    store->cursor = free_index + 1u == store->capacity ? 0 : free_index + 1u;

    out->slot = store->first_slot + free_index;
    out->generation = block->generation[lane];
    unlock_store(store);
    return 1;
}

int ores_native_execution_context_store_get(
        OresNativeExecutionContextStore* store,
        OresNativeExecutionHandle handle,
        OresNativeExecutionContext* out) {
    if (!store || !out || !lock_store(store)) return 0;
    uint32_t lane = 0, index = 0;
    ExecutionBlock* block = NULL;
    const int ok = resolve_locked(store, handle, &block, &lane, &index);
    if (ok) *out = load_context(block, lane);
    unlock_store(store);
    return ok;
}

int ores_native_execution_context_store_retain(
        OresNativeExecutionContextStore* store,
        OresNativeExecutionHandle handle) {
    if (!store || !lock_store(store)) return 0;
    uint32_t lane = 0, index = 0;
    ExecutionBlock* block = NULL;
    int ok = resolve_locked(store, handle, &block, &lane, &index)
        && block->refs[lane] != UINT32_MAX;
    if (ok) ++block->refs[lane];
    unlock_store(store);
    return ok;
}

int ores_native_execution_context_store_release(
        OresNativeExecutionContextStore* store,
        OresNativeExecutionHandle handle) {
    if (!store || !lock_store(store)) return 0;
    OresNativeExecutionHandle current = handle;
    int first = 1;
    int ok = 1;

    while (ores_native_store_handle_valid(current)) {
        uint32_t lane = 0, index = 0;
        ExecutionBlock* block = NULL;
        if (!resolve_locked(store, current, &block, &lane, &index)
                || block->refs[lane] == 0) {
            ok = 0;
            break;
        }
        first = 0;
        if (block->refs[lane] > 1) {
            --block->refs[lane];
            break;
        }

        OresNativeExecutionHandle parent = {
            block->parent_slot[lane], block->parent_generation[lane]
        };
        block->refs[lane] = 0;
        block->live[lane] = 0;
        block->runtime_domain[lane] = 0;
        block->execution[lane] = 0;
        block->allocation_kind[lane] = 0;
        block->allocation_domain[lane] = 0;
        block->allocation_owner[lane] = 0;
        block->allocation_generation[lane] = 0;
        block->parent_slot[lane] = 0;
        block->parent_generation[lane] = 0;
        block->continuation_slot[lane] = 0;
        block->continuation_generation[lane] = 0;
        if (block->generation[lane] == UINT32_MAX)
            block->generation[lane] = 0; /* permanently retired */
        else
            ++block->generation[lane];
        if (store->live_count == 0) abort();
        --store->live_count;
        store->cursor = index;
        current = parent;
    }

    unlock_store(store);
    return ok && !first;
}

int ores_native_execution_context_store_bind_continuation(
        OresNativeExecutionContextStore* store,
        OresNativeExecutionHandle execution,
        OresNativeContinuationHandle continuation) {
    if (!store || !ores_native_store_handle_valid(continuation)
            || !lock_store(store))
        return 0;
    uint32_t lane = 0, index = 0;
    ExecutionBlock* block = NULL;
    int ok = resolve_locked(store, execution, &block, &lane, &index);
    if (ok) {
        OresNativeContinuationHandle existing = {
            block->continuation_slot[lane],
            block->continuation_generation[lane]
        };
        if (!ores_native_store_handle_valid(existing)) {
            block->continuation_slot[lane] = continuation.slot;
            block->continuation_generation[lane] = continuation.generation;
        } else if (!same_handle(existing, continuation)) {
            ok = 0;
        }
    }
    unlock_store(store);
    return ok;
}

uint32_t ores_native_execution_context_store_capacity(
        OresNativeExecutionContextStore* store) {
    if (!store) return 0;
    return store->capacity;
}

uint32_t ores_native_execution_context_store_live(
        OresNativeExecutionContextStore* store) {
    if (!store || !lock_store(store)) return 0;
    const uint32_t live = store->live_count;
    unlock_store(store);
    return live;
}
