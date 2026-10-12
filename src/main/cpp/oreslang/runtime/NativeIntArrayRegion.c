#include "oreslang/runtime/NativeIntArrayRegion.h"
#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdlib.h>

enum { MAX_ARRAY_SLOTS = 64, MAX_ARRAY_ELEMENTS = 65536, REGION_ELEMENT_BUDGET = 262144 };

typedef struct {
    int64_t* values;
    uint64_t capacity;
    uint64_t length;
    uint64_t token;
} IntArraySlot;

struct OresNativeIntArrayRegion {
    uint64_t live_count;
    uint64_t reserved_elements;
    IntArraySlot arrays[MAX_ARRAY_SLOTS];
};

static _Atomic uint64_t next_array_token = 1;
static void fail_closed(void) { abort(); }

static uint64_t allocate_token(void) {
    uint64_t candidate = atomic_load_explicit(&next_array_token, memory_order_relaxed);
    while (candidate != 0 && candidate != UINT64_MAX) {
        if (atomic_compare_exchange_weak_explicit(
                    &next_array_token, &candidate, candidate + 1,
                    memory_order_relaxed, memory_order_relaxed))
            return candidate;
    }
    fail_closed();
    return 0;
}

#ifdef ORESLANG_NATIVE_TEST_TOKEN_EXHAUSTION
void oreslang_int_array_test_set_next_token(uint64_t value) {
    atomic_store_explicit(&next_array_token, value, memory_order_relaxed);
}
#endif

static void erase_bytes(void* data, size_t count) {
    volatile unsigned char* bytes = (volatile unsigned char*)data;
    for (size_t i = 0; i < count; ++i) bytes[i] = 0;
}

static IntArraySlot* require_slot(OresNativeIntArrayRegion* region,
                                  OresNativeIntArrayHandle handle) {
    if (!region || handle == 0 || handle == UINT64_MAX) fail_closed();
    for (size_t i = 0; i < MAX_ARRAY_SLOTS; ++i) {
        IntArraySlot* slot = &region->arrays[i];
        if (slot->token == handle) return slot;
    }
    fail_closed();
    return NULL;
}

OresNativeIntArrayRegion* oreslang_int_array_region_open(void) {
    OresNativeIntArrayRegion* region = (OresNativeIntArrayRegion*)calloc(
            1, sizeof(OresNativeIntArrayRegion));
    if (!region) fail_closed();
    return region;
}

OresNativeIntArrayHandle oreslang_int_array_new(OresNativeIntArrayRegion* region) {
    if (!region || region->live_count >= MAX_ARRAY_SLOTS) fail_closed();
    for (size_t i = 0; i < MAX_ARRAY_SLOTS; ++i) {
        IntArraySlot* slot = &region->arrays[i];
        if (slot->token == 0) {
            const uint64_t token = allocate_token();
            slot->token = token;
            ++region->live_count;
            return token;
        }
    }
    fail_closed();
    return 0;
}

void oreslang_int_array_reserve(OresNativeIntArrayRegion* region,
                               OresNativeIntArrayHandle handle, uint64_t capacity) {
    IntArraySlot* slot = require_slot(region, handle);
    if (capacity > MAX_ARRAY_ELEMENTS) fail_closed();
    if (capacity <= slot->capacity) return;
    const uint64_t additional = capacity - slot->capacity;
    if (additional > REGION_ELEMENT_BUDGET - region->reserved_elements)
        fail_closed();
    if (capacity > SIZE_MAX / sizeof(int64_t)) fail_closed();
    int64_t* grown = (int64_t*)realloc(slot->values,
                                      (size_t)capacity * sizeof(int64_t));
    if (!grown) fail_closed();
    for (uint64_t i = slot->capacity; i < capacity; ++i) grown[i] = 0;
    region->reserved_elements += additional;
    slot->capacity = capacity;
    slot->values = grown;
}

void oreslang_int_array_add(OresNativeIntArrayRegion* region,
                           OresNativeIntArrayHandle handle, int64_t value) {
    IntArraySlot* slot = require_slot(region, handle);
    if (slot->length == MAX_ARRAY_ELEMENTS) fail_closed();
    if (slot->length == slot->capacity) {
        uint64_t capacity = slot->capacity == 0 ? 4 : slot->capacity * 2;
        if (capacity > MAX_ARRAY_ELEMENTS) capacity = MAX_ARRAY_ELEMENTS;
        oreslang_int_array_reserve(region, handle, capacity);
    }
    slot->values[slot->length] = value;
    ++slot->length;
}

uint64_t oreslang_int_array_length(OresNativeIntArrayRegion* region,
                                  OresNativeIntArrayHandle handle) {
    return require_slot(region, handle)->length;
}

int64_t oreslang_int_array_get(OresNativeIntArrayRegion* region,
                              OresNativeIntArrayHandle handle, uint64_t index) {
    IntArraySlot* slot = require_slot(region, handle);
    if (index >= slot->length) fail_closed();
    return slot->values[index];
}

void oreslang_int_array_set(OresNativeIntArrayRegion* region,
                           OresNativeIntArrayHandle handle, uint64_t index,
                           int64_t value) {
    IntArraySlot* slot = require_slot(region, handle);
    if (index >= slot->length) fail_closed();
    slot->values[index] = value;
}

void oreslang_int_array_drop(OresNativeIntArrayRegion* region,
                            OresNativeIntArrayHandle handle) {
    IntArraySlot* slot = require_slot(region, handle);
    if (!region->live_count || slot->capacity > region->reserved_elements)
        fail_closed();
    if (slot->values) {
        erase_bytes(slot->values, (size_t)slot->capacity * sizeof(int64_t));
        free(slot->values);
    }
    region->reserved_elements -= slot->capacity;
    --region->live_count;
    erase_bytes(slot, sizeof(*slot));
}

void oreslang_int_array_region_close(OresNativeIntArrayRegion* region) {
    if (!region || region->live_count || region->reserved_elements)
        fail_closed();
    erase_bytes(region, sizeof(*region));
    free(region);
}
