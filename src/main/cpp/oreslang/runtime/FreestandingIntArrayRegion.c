#include "oreslang/runtime/AllocatorProviderV1.h"
#include "oreslang/runtime/NativeIntArrayRegion.h"

#include <stddef.h>
#include <stdint.h>

/*
 * Freestanding single-owner Array<int> runtime.
 *
 * This file intentionally exports the same trusted ABI symbols as the hosted
 * NativeIntArrayRegion implementation, but obtains every byte from the
 * explicitly bound AllocatorProviderV1. It never calls malloc/calloc/realloc,
 * free, libc I/O, an OS API, TLS, or a scheduler.
 */
enum {
    MAX_ARRAY_SLOTS = 16,
    MAX_ARRAY_ELEMENTS = 1024,
    REGION_ELEMENT_BUDGET = 4096
};

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

/* Zero-initialized so freestanding startup needs no .data copy phase. */
static uint64_t next_array_token;

#if defined(__GNUC__) || defined(__clang__)
__attribute__((noreturn))
#endif
static void fail_closed(void) {
#if defined(__GNUC__) || defined(__clang__)
    __builtin_trap();
#endif
    for (;;) {}
}

void oreslang_freestanding_panic(void) {
    fail_closed();
}

/*
 * ARM EHABI personality symbols are sometimes retained by Clang/LLD even
 * when unwind tables are disabled/discarded. Freestanding Oreslang never
 * unwinds: own these symbols locally and trap if control ever reaches them,
 * rather than acquiring an ambient libunwind dependency.
 */
#if defined(__arm__) || defined(__thumb__)
__attribute__((noreturn)) void __aeabi_unwind_cpp_pr0(void) {
    fail_closed();
}
__attribute__((noreturn)) void __aeabi_unwind_cpp_pr1(void) {
    fail_closed();
}
__attribute__((noreturn)) void __aeabi_unwind_cpp_pr2(void) {
    fail_closed();
}
#endif

static void erase_bytes(void* data, size_t count) {
    volatile uint8_t* bytes = (volatile uint8_t*)data;
    for (size_t i = 0; i < count; ++i) bytes[i] = 0;
}

static uint64_t allocate_token(void) {
    if (next_array_token == UINT64_MAX)
        fail_closed();
    ++next_array_token;
    if (next_array_token == 0)
        fail_closed();
    return next_array_token;
}

static void validate_region_accounting(
        const OresNativeIntArrayRegion* region) {
    if (!region || region->live_count > MAX_ARRAY_SLOTS ||
            region->reserved_elements > REGION_ELEMENT_BUDGET)
        fail_closed();
    uint64_t live = 0;
    uint64_t reserved = 0;
    for (size_t i = 0; i < MAX_ARRAY_SLOTS; ++i) {
        const IntArraySlot* slot = &region->arrays[i];
        if (slot->token == 0) {
            if (slot->values || slot->capacity || slot->length)
                fail_closed();
            continue;
        }
        if (slot->token == UINT64_MAX ||
                slot->capacity > MAX_ARRAY_ELEMENTS ||
                slot->length > slot->capacity ||
                (slot->values == 0) != (slot->capacity == 0))
            fail_closed();
        ++live;
        reserved += slot->capacity;
        if (reserved > REGION_ELEMENT_BUDGET) fail_closed();
    }
    if (live != region->live_count ||
            reserved != region->reserved_elements)
        fail_closed();
}

static IntArraySlot* require_slot(
        OresNativeIntArrayRegion* region,
        OresNativeIntArrayHandle handle) {
    if (!region || handle == 0 || handle == UINT64_MAX)
        fail_closed();
    for (size_t i = 0; i < MAX_ARRAY_SLOTS; ++i) {
        IntArraySlot* slot = &region->arrays[i];
        if (slot->token != handle) continue;
        if (slot->length > slot->capacity ||
                slot->capacity > MAX_ARRAY_ELEMENTS ||
                (slot->values == 0) != (slot->capacity == 0))
            fail_closed();
        return slot;
    }
    fail_closed();
    return 0;
}

static void erase_backing(IntArraySlot* slot) {
    if (slot->values && slot->capacity)
        erase_bytes(
            slot->values,
            (size_t)slot->capacity * sizeof(int64_t));
}

void oreslang_int_array_unhandled_resource_failure(uint32_t status) {
    if (status != ORES_NATIVE_ARRAY_LIMIT &&
            status != ORES_NATIVE_ARRAY_NO_MEMORY)
        fail_closed();
    fail_closed();
}

OresNativeIntArrayStatus oreslang_int_array_try_region_open(
        OresNativeIntArrayRegion** out_region) {
    if (!out_region) fail_closed();
    OresAllocatorProviderV1* provider =
        ores_allocator_provider_v1_current();
    if (!provider) return ORES_NATIVE_ARRAY_NO_MEMORY;
    OresNativeIntArrayRegion* region =
        (OresNativeIntArrayRegion*)ores_allocator_provider_v1_alloc(
            provider,
            sizeof(OresNativeIntArrayRegion),
            sizeof(uint64_t));
    if (!region) return ORES_NATIVE_ARRAY_NO_MEMORY;
    *out_region = region;
    return ORES_NATIVE_ARRAY_OK;
}

OresNativeIntArrayRegion* oreslang_int_array_region_open(void) {
    OresNativeIntArrayRegion* region = 0;
    if (oreslang_int_array_try_region_open(&region) !=
            ORES_NATIVE_ARRAY_OK)
        fail_closed();
    return region;
}

OresNativeIntArrayStatus oreslang_int_array_try_new(
        OresNativeIntArrayRegion* region,
        OresNativeIntArrayHandle* out_handle) {
    if (!region || !out_handle) fail_closed();
    validate_region_accounting(region);
    if (region->live_count == MAX_ARRAY_SLOTS)
        return ORES_NATIVE_ARRAY_LIMIT;
    for (size_t i = 0; i < MAX_ARRAY_SLOTS; ++i) {
        IntArraySlot* slot = &region->arrays[i];
        if (slot->token != 0) continue;
        if (slot->values || slot->capacity || slot->length)
            fail_closed();
        slot->token = allocate_token();
        ++region->live_count;
        *out_handle = slot->token;
        return ORES_NATIVE_ARRAY_OK;
    }
    fail_closed();
    return ORES_NATIVE_ARRAY_LIMIT;
}

OresNativeIntArrayHandle oreslang_int_array_new(
        OresNativeIntArrayRegion* region) {
    OresNativeIntArrayHandle handle = 0;
    if (oreslang_int_array_try_new(region, &handle) !=
            ORES_NATIVE_ARRAY_OK)
        fail_closed();
    return handle;
}

OresNativeIntArrayStatus oreslang_int_array_try_reserve(
        OresNativeIntArrayRegion* region,
        OresNativeIntArrayHandle handle,
        uint64_t capacity) {
    IntArraySlot* slot = require_slot(region, handle);
    validate_region_accounting(region);
    if (capacity > MAX_ARRAY_ELEMENTS ||
            capacity > (uint64_t)(SIZE_MAX / sizeof(int64_t)))
        return ORES_NATIVE_ARRAY_LIMIT;
    if (capacity <= slot->capacity)
        return ORES_NATIVE_ARRAY_OK;

    const uint64_t additional = capacity - slot->capacity;
    if (additional > REGION_ELEMENT_BUDGET -
            region->reserved_elements)
        return ORES_NATIVE_ARRAY_LIMIT;

    OresAllocatorProviderV1* provider =
        ores_allocator_provider_v1_current();
    if (!provider) return ORES_NATIVE_ARRAY_NO_MEMORY;
    int64_t* grown = (int64_t*)ores_allocator_provider_v1_alloc(
        provider,
        (size_t)capacity * sizeof(int64_t),
        sizeof(uint64_t));
    if (!grown) return ORES_NATIVE_ARRAY_NO_MEMORY;

    for (uint64_t i = 0; i < slot->length; ++i)
        grown[i] = slot->values[i];
    erase_backing(slot);
    slot->values = grown;
    region->reserved_elements += additional;
    slot->capacity = capacity;
    validate_region_accounting(region);
    return ORES_NATIVE_ARRAY_OK;
}

void oreslang_int_array_reserve(
        OresNativeIntArrayRegion* region,
        OresNativeIntArrayHandle handle,
        uint64_t capacity) {
    if (oreslang_int_array_try_reserve(region, handle, capacity) !=
            ORES_NATIVE_ARRAY_OK)
        fail_closed();
}

OresNativeIntArrayStatus oreslang_int_array_try_add(
        OresNativeIntArrayRegion* region,
        OresNativeIntArrayHandle handle,
        int64_t value) {
    IntArraySlot* slot = require_slot(region, handle);
    if (slot->length == MAX_ARRAY_ELEMENTS)
        return ORES_NATIVE_ARRAY_LIMIT;
    if (slot->length == slot->capacity) {
        uint64_t next = slot->capacity == 0 ? 4 : slot->capacity * 2;
        if (next > MAX_ARRAY_ELEMENTS) next = MAX_ARRAY_ELEMENTS;
        const OresNativeIntArrayStatus status =
            oreslang_int_array_try_reserve(region, handle, next);
        if (status != ORES_NATIVE_ARRAY_OK) return status;
        slot = require_slot(region, handle);
    }
    slot->values[slot->length++] = value;
    return ORES_NATIVE_ARRAY_OK;
}

void oreslang_int_array_add(
        OresNativeIntArrayRegion* region,
        OresNativeIntArrayHandle handle,
        int64_t value) {
    if (oreslang_int_array_try_add(region, handle, value) !=
            ORES_NATIVE_ARRAY_OK)
        fail_closed();
}

uint64_t oreslang_int_array_length(
        OresNativeIntArrayRegion* region,
        OresNativeIntArrayHandle handle) {
    return require_slot(region, handle)->length;
}

int64_t oreslang_int_array_get(
        OresNativeIntArrayRegion* region,
        OresNativeIntArrayHandle handle,
        uint64_t index) {
    IntArraySlot* slot = require_slot(region, handle);
    if (index >= slot->length) fail_closed();
    return slot->values[index];
}

void oreslang_int_array_set(
        OresNativeIntArrayRegion* region,
        OresNativeIntArrayHandle handle,
        uint64_t index,
        int64_t value) {
    IntArraySlot* slot = require_slot(region, handle);
    if (index >= slot->length) fail_closed();
    slot->values[index] = value;
}

void oreslang_int_array_drop(
        OresNativeIntArrayRegion* region,
        OresNativeIntArrayHandle handle) {
    IntArraySlot* slot = require_slot(region, handle);
    validate_region_accounting(region);
    const uint64_t capacity = slot->capacity;
    erase_backing(slot);
    erase_bytes(slot, sizeof(*slot));
    --region->live_count;
    region->reserved_elements -= capacity;
    validate_region_accounting(region);
}

OresNativeIntArrayStatus oreslang_int_array_try_prepare_many(
        OresNativeIntArrayRegion* region,
        uint64_t n,
        OresNativeIntArrayHandle* out) {
    if (!region || (n && !out) || n > MAX_ARRAY_SLOTS)
        fail_closed();
    OresNativeIntArrayHandle staged[MAX_ARRAY_SLOTS];
    uint64_t prepared = 0;
    for (; prepared < n; ++prepared) {
        const OresNativeIntArrayStatus status =
            oreslang_int_array_try_new(
                region, &staged[prepared]);
        if (status != ORES_NATIVE_ARRAY_OK) {
            while (prepared != 0) {
                --prepared;
                oreslang_int_array_drop(
                    region, staged[prepared]);
            }
            return status;
        }
    }
    for (uint64_t i = 0; i < n; ++i) out[i] = staged[i];
    return ORES_NATIVE_ARRAY_OK;
}

OresNativeIntArrayStatus
oreslang_int_array_try_prepare_filled_many(
        OresNativeIntArrayRegion* region,
        uint64_t n,
        const uint64_t lengths[],
        const int64_t values[],
        OresNativeIntArrayHandle out[]) {
    if (!region || n > MAX_ARRAY_SLOTS ||
            (n && (!lengths || !values || !out)))
        fail_closed();
    uint64_t requested = 0;
    for (uint64_t i = 0; i < n; ++i) {
        if (lengths[i] > MAX_ARRAY_ELEMENTS)
            return ORES_NATIVE_ARRAY_LIMIT;
        requested += lengths[i];
        if (requested > REGION_ELEMENT_BUDGET -
                region->reserved_elements)
            return ORES_NATIVE_ARRAY_LIMIT;
    }

    OresNativeIntArrayHandle staged[MAX_ARRAY_SLOTS];
    uint64_t prepared = 0;
    for (; prepared < n; ++prepared) {
        OresNativeIntArrayStatus status =
            oreslang_int_array_try_new(
                region, &staged[prepared]);
        const int acquired_current =
            status == ORES_NATIVE_ARRAY_OK;
        if (status == ORES_NATIVE_ARRAY_OK &&
                lengths[prepared] != 0)
            status = oreslang_int_array_try_reserve(
                region, staged[prepared], lengths[prepared]);
        if (status != ORES_NATIVE_ARRAY_OK) {
            if (acquired_current)
                oreslang_int_array_drop(
                    region, staged[prepared]);
            while (prepared != 0) {
                --prepared;
                oreslang_int_array_drop(
                    region, staged[prepared]);
            }
            return status;
        }
        IntArraySlot* slot =
            require_slot(region, staged[prepared]);
        for (uint64_t j = 0; j < lengths[prepared]; ++j)
            slot->values[j] = values[prepared];
        slot->length = lengths[prepared];
    }
    for (uint64_t i = 0; i < n; ++i) out[i] = staged[i];
    return ORES_NATIVE_ARRAY_OK;
}

void oreslang_int_array_commit_many(
        OresNativeIntArrayRegion* region,
        uint64_t n,
        OresNativeIntArrayHandle* const destinations[],
        OresNativeIntArrayHandle* const sources[]) {
    if (!region || n > MAX_ARRAY_SLOTS ||
            (n && (!destinations || !sources)))
        fail_closed();
    validate_region_accounting(region);
    OresNativeIntArrayHandle old[MAX_ARRAY_SLOTS];
    OresNativeIntArrayHandle next[MAX_ARRAY_SLOTS];
    for (uint64_t i = 0; i < n; ++i) {
        if (!destinations[i] || !sources[i] ||
                destinations[i] == sources[i])
            fail_closed();
        old[i] = *destinations[i];
        next[i] = *sources[i];
        (void)require_slot(region, next[i]);
        if (old[i]) (void)require_slot(region, old[i]);
        if (next[i] == old[i]) fail_closed();
        for (uint64_t j = 0; j < i; ++j) {
            if (destinations[i] == destinations[j] ||
                    sources[i] == sources[j] ||
                    destinations[i] == sources[j] ||
                    sources[i] == destinations[j] ||
                    next[i] == next[j] ||
                    (old[i] && old[i] == old[j]))
                fail_closed();
        }
    }
    for (uint64_t i = 0; i < n; ++i)
        for (uint64_t j = 0; j < n; ++j)
            if (old[i] && old[i] == next[j])
                fail_closed();

    for (uint64_t i = 0; i < n; ++i)
        if (old[i]) oreslang_int_array_drop(region, old[i]);
    for (uint64_t i = 0; i < n; ++i) {
        *destinations[i] = next[i];
        *sources[i] = 0;
    }
}

OresNativeIntArrayStatus oreslang_int_array_try_transfer_many(
        OresNativeIntArrayRegion* source_region,
        OresNativeIntArrayRegion* destination_region,
        uint64_t n,
        OresNativeIntArrayHandle* const destinations[],
        OresNativeIntArrayHandle* const sources[]) {
    if (!source_region || !destination_region ||
            source_region == destination_region ||
            n > MAX_ARRAY_SLOTS ||
            (n && (!destinations || !sources)))
        fail_closed();
    validate_region_accounting(source_region);
    validate_region_accounting(destination_region);

    IntArraySlot* from[MAX_ARRAY_SLOTS];
    IntArraySlot* into[MAX_ARRAY_SLOTS];
    uint64_t reserved = 0;
    for (uint64_t i = 0; i < n; ++i) {
        if (!destinations[i] || !sources[i] ||
                *destinations[i] ||
                destinations[i] == sources[i])
            fail_closed();
        from[i] = require_slot(source_region, *sources[i]);
        reserved += from[i]->capacity;
        for (uint64_t j = 0; j < i; ++j)
            if (from[i] == from[j] ||
                    destinations[i] == destinations[j] ||
                    sources[i] == sources[j] ||
                    destinations[i] == sources[j] ||
                    sources[i] == destinations[j])
                fail_closed();
    }
    if (n > MAX_ARRAY_SLOTS -
            destination_region->live_count ||
            reserved > REGION_ELEMENT_BUDGET -
                destination_region->reserved_elements)
        return ORES_NATIVE_ARRAY_LIMIT;

    uint64_t empty = 0;
    for (size_t i = 0;
            i < MAX_ARRAY_SLOTS && empty < n; ++i) {
        if (!destination_region->arrays[i].token)
            into[empty++] = &destination_region->arrays[i];
    }
    if (empty != n) fail_closed();

    for (uint64_t i = 0; i < n; ++i) {
        *into[i] = *from[i];
        erase_bytes(from[i], sizeof(*from[i]));
        *destinations[i] = into[i]->token;
        *sources[i] = 0;
    }
    source_region->live_count -= n;
    source_region->reserved_elements -= reserved;
    destination_region->live_count += n;
    destination_region->reserved_elements += reserved;
    validate_region_accounting(source_region);
    validate_region_accounting(destination_region);
    return ORES_NATIVE_ARRAY_OK;
}

void oreslang_int_array_region_close(
        OresNativeIntArrayRegion* region) {
    validate_region_accounting(region);
    if (region->live_count || region->reserved_elements)
        fail_closed();
    erase_bytes(region, sizeof(*region));
}
