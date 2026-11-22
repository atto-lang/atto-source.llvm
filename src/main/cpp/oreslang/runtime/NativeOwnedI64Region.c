#include "oreslang/runtime/NativeOwnedI64Region.h"
#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdlib.h>

typedef struct {
    int64_t value;
    uint64_t token;
} OwnedI64Slot;

struct OresOwnedI64Region {
    uint64_t capacity;
    uint64_t live_count;
    OwnedI64Slot slots[];
};

/* Lifetime tokens never repeat across slot or region reuse. */
static _Atomic uint64_t next_token = 1;
static void fail_closed(void) { abort(); }
static uint64_t allocate_token(void) {
    uint64_t candidate = atomic_load_explicit(&next_token, memory_order_relaxed);
    while (candidate != 0 && candidate != UINT64_MAX) {
        if (atomic_compare_exchange_weak_explicit(&next_token, &candidate,
                candidate + 1, memory_order_relaxed, memory_order_relaxed))
            return candidate;
    }
    fail_closed();
    return 0;
}
#ifdef ORESLANG_NATIVE_TEST_TOKEN_EXHAUSTION
void oreslang_owned_i64_test_set_next_token(uint64_t next) {
    atomic_store_explicit(&next_token, next, memory_order_relaxed);
}
#endif
static void erase_bytes(void* address, size_t length) {
    volatile unsigned char* bytes = (volatile unsigned char*)address;
    for (size_t i = 0; i < length; ++i) bytes[i] = 0;
}
static OwnedI64Slot* require_slot(OresOwnedI64Region* region, uint64_t token) {
    if (!region || token == 0 || token == UINT64_MAX) fail_closed();
    for (uint64_t i = 0; i < region->capacity; ++i) {
        OwnedI64Slot* slot = &region->slots[i];
        if (slot->token == token) return slot;
    }
    fail_closed();
    return NULL;
}
OresOwnedI64Region* oreslang_owned_i64_region_open(uint64_t slots) {
    if (slots == 0 || slots > 4096) fail_closed();
    if (slots > (SIZE_MAX - offsetof(OresOwnedI64Region, slots)) / sizeof(OwnedI64Slot))
        fail_closed();
    const size_t bytes = offsetof(OresOwnedI64Region, slots)
                       + (size_t)slots * sizeof(OwnedI64Slot);
    OresOwnedI64Region* region = (OresOwnedI64Region*)calloc(1, bytes);
    if (!region) fail_closed();
    region->capacity = slots;
    return region;
}
OresOwnedI64Handle oreslang_owned_i64_region_new(OresOwnedI64Region* region, int64_t value) {
    if (!region || region->live_count >= region->capacity) fail_closed();
    for (uint64_t i = 0; i < region->capacity; ++i) {
        OwnedI64Slot* slot = &region->slots[i];
        if (slot->token == 0) {
            uint64_t token = allocate_token();
            slot->value = value;
            slot->token = token;
            ++region->live_count;
            return token;
        }
    }
    fail_closed();
    return 0;
}
int64_t oreslang_owned_i64_region_read(OresOwnedI64Region* region, OresOwnedI64Handle handle) {
    return require_slot(region, handle)->value;
}
void oreslang_owned_i64_region_drop(OresOwnedI64Region* region, OresOwnedI64Handle handle) {
    if (!region || region->live_count == 0) fail_closed();
    OwnedI64Slot* slot = require_slot(region, handle);
    erase_bytes(slot, sizeof(*slot));
    --region->live_count;
}
void oreslang_owned_i64_region_close(OresOwnedI64Region* region) {
    if (!region || region->live_count != 0) fail_closed();
    const size_t bytes = offsetof(OresOwnedI64Region, slots)
                       + (size_t)region->capacity * sizeof(OwnedI64Slot);
    erase_bytes(region, bytes);
    free(region);
}
