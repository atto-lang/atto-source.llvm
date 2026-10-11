#include "oreslang/runtime/NativeOwnedI64Region.h"
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    int64_t value;
    unsigned char alive;
    uint64_t generation;
} OwnedI64Slot;

struct OresOwnedI64Region {
    uint64_t capacity;
    uint64_t live_count;
    uint64_t handle_only;
    OwnedI64Slot slots[];
};

static void fail_closed(void) { abort(); }

/* An ordinary memset immediately before free can be elided by optimization.
 * Volatile writes are observable and wipe all remaining region metadata and
 * former payload bytes before returning storage to the allocator. */
static void erase_bytes(void* address, size_t length) {
    volatile unsigned char* const bytes = (volatile unsigned char*) address;
    for (size_t i = 0; i < length; ++i) bytes[i] = 0;
}

OresOwnedI64Region* oreslang_owned_i64_region_open(uint64_t slots) {
    if (slots == 0 || slots > 4096) fail_closed();
    if (slots > (SIZE_MAX - offsetof(OresOwnedI64Region, slots)) / sizeof(OwnedI64Slot))
        fail_closed();
    const size_t bytes = offsetof(OresOwnedI64Region, slots)
                       + (size_t) slots * sizeof(OwnedI64Slot);
    OresOwnedI64Region* region = (OresOwnedI64Region*) calloc(1, bytes);
    if (!region) fail_closed(); /* Never hand unchecked null to generated code. */
    region->capacity = slots;
    return region;
}

int64_t* oreslang_owned_i64_region_new(OresOwnedI64Region* region, int64_t value) {
    if (!region || region->handle_only || region->live_count >= region->capacity) fail_closed();
    for (uint64_t i = 0; i < region->capacity; ++i) {
        OwnedI64Slot* slot = &region->slots[i];
        if (!slot->alive) {
            slot->value = value;
            slot->alive = 1;
            region->live_count++;
            return &slot->value;
        }
    }
    fail_closed();
    return NULL;
}

void oreslang_owned_i64_region_drop(OresOwnedI64Region* region, int64_t* value) {
    if (!region || region->handle_only || !value || region->live_count == 0) fail_closed();
    /* Provenance check before dereference. Exact slot value address only. */
    const uintptr_t first = (uintptr_t) &region->slots[0].value;
    const uintptr_t addr = (uintptr_t) value;
    const size_t stride = sizeof(OwnedI64Slot);
    if (addr < first || ((addr - first) % stride) != 0) fail_closed();
    const uint64_t index = (addr - first) / stride;
    if (index >= region->capacity) fail_closed();
    OwnedI64Slot* slot = &region->slots[index];
    if (!slot->alive || &slot->value != value) fail_closed();
    erase_bytes(&slot->value, sizeof(slot->value));
    slot->alive = 0;
    region->live_count--;
}

void oreslang_owned_i64_region_close(OresOwnedI64Region* region) {
    if (!region || region->live_count != 0) fail_closed();
    const size_t bytes = offsetof(OresOwnedI64Region, slots)
                       + (size_t) region->capacity * sizeof(OwnedI64Slot);
    erase_bytes(region, bytes);
    free(region); /* Native allocator only; no collector/statepoints. */
}

/* Opaque by-value i64 handle: [52-bit allocation generation][12-bit slot].
 * The full generation is checked on every access. No pointer/address is
 * exposed to generated guest code; exhaustion permanently retires a slot. */
#define ORES_REGION_SLOT_BITS UINT64_C(12)
#define ORES_REGION_SLOT_MASK UINT64_C(4095)
#define ORES_REGION_MAX_GENERATION (UINT64_MAX >> ORES_REGION_SLOT_BITS)
static OwnedI64Slot* checked_handle(OresOwnedI64Region* region, uint64_t handle) {
    if (!region || !region->handle_only || handle == 0) fail_closed();
    const uint64_t index = handle & ORES_REGION_SLOT_MASK;
    const uint64_t generation = handle >> ORES_REGION_SLOT_BITS;
    if (index >= region->capacity || generation == 0) fail_closed();
    OwnedI64Slot* slot = &region->slots[index];
    if (!slot->alive || slot->generation != generation) fail_closed();
    return slot;
}
OresOwnedI64Region* oreslang_owned_i64_region_open_handles(uint64_t slots) {
    OresOwnedI64Region* region = oreslang_owned_i64_region_open(slots);
    region->handle_only = 1;
    return region;
}
uint64_t oreslang_owned_i64_region_new_handle(OresOwnedI64Region* region, int64_t value) {
    if (!region || !region->handle_only || region->live_count >= region->capacity)
        fail_closed();
    for (uint64_t index = 0; index < region->capacity; ++index) {
        OwnedI64Slot* slot = &region->slots[index];
        if (slot->alive || slot->generation >= ORES_REGION_MAX_GENERATION)
            continue;
        ++slot->generation;
        slot->value = value;
        slot->alive = 1;
        ++region->live_count;
        return (slot->generation << ORES_REGION_SLOT_BITS) | index;
    }
    fail_closed();
    return 0;
}
int64_t oreslang_owned_i64_region_read_handle(OresOwnedI64Region* region,
                                              uint64_t handle) {
    return checked_handle(region, handle)->value;
}
void oreslang_owned_i64_region_drop_handle(OresOwnedI64Region* region, uint64_t handle) {
    OwnedI64Slot* slot = checked_handle(region, handle);
    erase_bytes(&slot->value, sizeof(slot->value));
    slot->alive = 0;
    --region->live_count;
}
#ifdef ORESLANG_NATIVE_TEST_REGION_GENERATION
void oreslang_owned_i64_region_test_set_generation(OresOwnedI64Region* region,
                                                    uint64_t slot, uint64_t generation) {
    if (!region || !region->handle_only || slot >= region->capacity ||
        region->slots[slot].alive || generation > ORES_REGION_MAX_GENERATION)
        fail_closed();
    region->slots[slot].generation = generation;
}
#endif
