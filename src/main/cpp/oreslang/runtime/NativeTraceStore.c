#include "oreslang/runtime/NativeTraceStore.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define LANES 8u
#define FIELDS 8u
#define NO_SLOT UINT32_MAX

typedef struct {
    uint64_t columns[FIELDS][LANES];
    uint32_t generations[LANES];
    uint32_t free_next[LANES];
    unsigned char live[LANES];
} TraceBlock;

struct OresNativeTraceStore {
    pthread_mutex_t mutex;
    TraceBlock* blocks;
    uint32_t capacity;
    uint32_t next_unused;
    uint32_t free_head;
    uint64_t id;
    int kind;
};

static _Atomic(uint64_t) next_store_id = 1;

static uint64_t reserve_store_id(void) {
    uint64_t prior = atomic_load(&next_store_id);
    while (prior && prior != UINT64_MAX) {
        if (atomic_compare_exchange_weak(&next_store_id, &prior, prior + 1))
            return prior;
    }
    return 0;
}

static TraceBlock* block_at(OresNativeTraceStore* store, uint32_t slot) {
    return &store->blocks[slot / LANES];
}

static OresNativeTraceStore* open_store(size_t capacity, int kind) {
    if (!capacity || capacity > UINT32_MAX - 1u) return NULL;
    size_t blocks = capacity / LANES + ((capacity % LANES) != 0);
    if (blocks > SIZE_MAX / sizeof(TraceBlock)) return NULL;
    OresNativeTraceStore* store = calloc(1, sizeof(*store));
    if (!store) return NULL;
    store->blocks = calloc(blocks, sizeof(TraceBlock));
    if (!store->blocks || pthread_mutex_init(&store->mutex, NULL) != 0) {
        free(store->blocks);
        free(store);
        return NULL;
    }
    store->id = reserve_store_id();
    if (!store->id) {
        pthread_mutex_destroy(&store->mutex);
        free(store->blocks);
        free(store);
        return NULL;
    }
    store->capacity = (uint32_t) capacity;
    store->free_head = NO_SLOT;
    store->kind = kind;
    return store;
}

OresNativeTraceStore* ores_native_execution_store_open(size_t capacity) {
    return open_store(capacity, 1);
}
OresNativeTraceStore* ores_native_continuation_store_open(size_t capacity) {
    return open_store(capacity, 2);
}
void ores_native_trace_store_close(OresNativeTraceStore* store) {
    if (!store) return;
    /* Caller must quiesce all users before close. */
    pthread_mutex_destroy(&store->mutex);
    free(store->blocks);
    free(store);
}

static OresNativeTraceHandle insert(
        OresNativeTraceStore* store, int kind, const void* record) {
    OresNativeTraceHandle invalid = {0};
    if (!store || store->kind != kind || !record) return invalid;
    if (pthread_mutex_lock(&store->mutex) != 0) abort();
    uint32_t slot;
    if (store->free_head != NO_SLOT) {
        slot = store->free_head;
        TraceBlock* block = block_at(store, slot);
        store->free_head = block->free_next[slot % LANES];
    } else if (store->next_unused < store->capacity) {
        slot = store->next_unused++;
    } else {
        pthread_mutex_unlock(&store->mutex);
        return invalid;
    }
    TraceBlock* block = block_at(store, slot);
    uint32_t lane = slot % LANES;
    if (!block->generations[lane]) block->generations[lane] = 1;
    uint64_t words[FIELDS];
    memcpy(words, record, sizeof(words));
    for (size_t i = 0; i < FIELDS; ++i) block->columns[i][lane] = words[i];
    block->live[lane] = 1;
    OresNativeTraceHandle result = {
        store->id, ((uint64_t)block->generations[lane] << 32) | (slot + 1u)
    };
    pthread_mutex_unlock(&store->mutex);
    return result;
}

OresNativeTraceHandle ores_native_execution_store_insert(
        OresNativeTraceStore* store, const OresNativeExecutionContext* value) {
    return insert(store, 1, value);
}
OresNativeTraceHandle ores_native_continuation_store_insert(
        OresNativeTraceStore* store, const OresNativeContinuationRecord* value) {
    return insert(store, 2, value);
}

static int resolve(OresNativeTraceStore* store, OresNativeTraceHandle h,
        uint32_t* slot, TraceBlock** block) {
    if (h.store_id != store->id || !h.slot_generation) return 0;
    uint32_t one_based = (uint32_t) h.slot_generation;
    uint32_t generation = (uint32_t)(h.slot_generation >> 32);
    if (!one_based || !generation || one_based > store->next_unused) return 0;
    *slot = one_based - 1;
    *block = block_at(store, *slot);
    uint32_t lane = *slot % LANES;
    return (*block)->live[lane] && (*block)->generations[lane] == generation;
}

static int load(OresNativeTraceStore* store, int kind,
        OresNativeTraceHandle handle, void* out) {
    if (!store || store->kind != kind || !out) return 0;
    if (pthread_mutex_lock(&store->mutex) != 0) abort();
    TraceBlock* block;
    uint32_t slot;
    int success = resolve(store, handle, &slot, &block);
    if (success) {
        uint64_t words[FIELDS];
        for (size_t i = 0; i < FIELDS; ++i)
            words[i] = block->columns[i][slot % LANES];
        memcpy(out, words, sizeof(words));
    }
    pthread_mutex_unlock(&store->mutex);
    return success;
}
int ores_native_execution_store_load(OresNativeTraceStore* s,
        OresNativeTraceHandle h, OresNativeExecutionContext* out) {
    return load(s, 1, h, out);
}
int ores_native_continuation_store_load(OresNativeTraceStore* s,
        OresNativeTraceHandle h, OresNativeContinuationRecord* out) {
    return load(s, 2, h, out);
}

int ores_native_trace_store_release(OresNativeTraceStore* store,
        OresNativeTraceHandle handle) {
    if (!store) return 0;
    if (pthread_mutex_lock(&store->mutex) != 0) abort();
    TraceBlock* block;
    uint32_t slot;
    int success = resolve(store, handle, &slot, &block);
    if (success) {
        uint32_t lane = slot % LANES;
        for (size_t i = 0; i < FIELDS; ++i)
            block->columns[i][lane] = 0;
        block->live[lane] = 0;
        /* Generation overflow permanently retires the slot. */
        if (block->generations[lane] != UINT32_MAX) {
            ++block->generations[lane];
            block->free_next[lane] = store->free_head;
            store->free_head = slot;
        }
    }
    pthread_mutex_unlock(&store->mutex);
    return success;
}
