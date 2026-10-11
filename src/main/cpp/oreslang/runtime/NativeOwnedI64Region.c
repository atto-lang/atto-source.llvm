#include "oreslang/runtime/NativeOwnedI64Region.h"
#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdlib.h>

/* A 52-bit globally unique generation and 12-bit slot. Each region has at
 * most 4096 slots, and the generation counter never wraps or resets. */
#define ORES_REGION_SLOT_BITS UINT64_C(12)
#define ORES_REGION_SLOT_MASK UINT64_C(4095)
#define ORES_REGION_MAX_GENERATION (UINT64_MAX >> ORES_REGION_SLOT_BITS)

typedef struct {
    int64_t value;
    uint64_t generation;
    unsigned char alive;
} OwnedI64Slot;

struct OresOwnedI64Region {
    uint64_t capacity;
    uint64_t live_count;
    /* Serialized slot access prevents read/drop and alloc/drop data races.
     * Closing the region still requires external lifetime quiescence. */
    atomic_flag gate;
    OwnedI64Slot slots[];
};

static _Atomic(uint64_t) next_region_generation = 1;
static void fail_closed(void) { abort(); }

static void erase_bytes(void* address, size_t length) {
    volatile unsigned char* bytes = (volatile unsigned char*)address;
    for (size_t i = 0; i < length; ++i) bytes[i] = 0;
}
static void lock_region(OresOwnedI64Region* region) {
    while (atomic_flag_test_and_set_explicit(&region->gate, memory_order_acquire)) {}
}
static void unlock_region(OresOwnedI64Region* region) {
    atomic_flag_clear_explicit(&region->gate, memory_order_release);
}
static uint64_t allocate_generation(void) {
    uint64_t candidate = atomic_load_explicit(&next_region_generation, memory_order_relaxed);
    while (candidate != 0 && candidate <= ORES_REGION_MAX_GENERATION) {
        if (atomic_compare_exchange_weak_explicit(&next_region_generation,
                &candidate, candidate + 1, memory_order_relaxed, memory_order_relaxed))
            return candidate;
    }
    fail_closed();
    return 0;
}
static OwnedI64Slot* checked_handle_locked(OresOwnedI64Region* region, uint64_t handle) {
    if (handle == 0) fail_closed();
    const uint64_t index = handle & ORES_REGION_SLOT_MASK;
    const uint64_t generation = handle >> ORES_REGION_SLOT_BITS;
    if (index >= region->capacity || generation == 0) fail_closed();
    OwnedI64Slot* slot = &region->slots[index];
    if (!slot->alive || slot->generation != generation) fail_closed();
    return slot;
}
OresOwnedI64Region* oreslang_owned_i64_region_open_handles(uint64_t slots) {
    if (slots == 0 || slots > 4096 ||
        slots > (SIZE_MAX - offsetof(OresOwnedI64Region, slots)) / sizeof(OwnedI64Slot))
        fail_closed();
    const size_t bytes = offsetof(OresOwnedI64Region, slots)
                       + (size_t)slots * sizeof(OwnedI64Slot);
    OresOwnedI64Region* region = (OresOwnedI64Region*)calloc(1, bytes);
    if (!region) fail_closed();
    region->capacity = slots;
    atomic_flag_clear(&region->gate);
    return region;
}
uint64_t oreslang_owned_i64_region_new_handle(OresOwnedI64Region* region, int64_t value) {
    if (!region) fail_closed();
    lock_region(region);
    if (region->live_count >= region->capacity) fail_closed();
    for (uint64_t index = 0; index < region->capacity; ++index) {
        OwnedI64Slot* slot = &region->slots[index];
        if (slot->alive) continue;
        const uint64_t generation = allocate_generation();
        slot->value = value;
        slot->generation = generation;
        slot->alive = 1;
        ++region->live_count;
        unlock_region(region);
        return (generation << ORES_REGION_SLOT_BITS) | index;
    }
    fail_closed();
    return 0;
}
int64_t oreslang_owned_i64_region_read_handle(OresOwnedI64Region* region, uint64_t handle) {
    if (!region) fail_closed();
    lock_region(region);
    const int64_t value = checked_handle_locked(region, handle)->value;
    unlock_region(region);
    return value;
}
void oreslang_owned_i64_region_drop_handle(OresOwnedI64Region* region, uint64_t handle) {
    if (!region) fail_closed();
    lock_region(region);
    OwnedI64Slot* slot = checked_handle_locked(region, handle);
    erase_bytes(&slot->value, sizeof(slot->value));
    slot->generation = 0;
    slot->alive = 0;
    --region->live_count;
    unlock_region(region);
}
void oreslang_owned_i64_region_close(OresOwnedI64Region* region) {
    /* All callers must have stopped using this region before close starts. */
    if (!region) fail_closed();
    lock_region(region);
    if (region->live_count != 0) fail_closed();
    const size_t bytes = offsetof(OresOwnedI64Region, slots)
                       + (size_t)region->capacity * sizeof(OwnedI64Slot);
    /* Never destroy/clear a locked atomic_flag; scrub payloads and metadata. */
    for (uint64_t i = 0; i < region->capacity; ++i)
        erase_bytes(&region->slots[i], sizeof(region->slots[i]));
    region->capacity = 0;
    region->live_count = 0;
    unlock_region(region);
    free(region);
    (void)bytes;
}
#ifdef ORESLANG_NATIVE_TEST_REGION_GENERATION
/* Test-only deterministic exhaustion injection; never compiled into runtime
 * executables used by Oreslang guest code. */
void oreslang_owned_i64_region_test_set_generation(OresOwnedI64Region* region,
                                                    uint64_t slot, uint64_t generation) {
    if (!region || slot >= region->capacity || region->slots[slot].alive ||
        generation == 0 || generation > ORES_REGION_MAX_GENERATION)
        fail_closed();
    atomic_store_explicit(&next_region_generation, generation, memory_order_relaxed);
}
#endif
