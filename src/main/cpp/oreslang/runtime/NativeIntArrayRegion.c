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

/* The status is a value-only fixed-width failure payload: no allocation,
 * no exception object, no guest pointer, and no handler reentry. A trusted
 * compiler invokes this only after it has closed/dropped its owned region.
 * The separate native error/supervisor dispatch is not available yet.
 */
void oreslang_int_array_unhandled_resource_failure(uint32_t status) {
    if (status != ORES_NATIVE_ARRAY_LIMIT &&
            status != ORES_NATIVE_ARRAY_NO_MEMORY)
        fail_closed();
    fail_closed(); /* Unhandled ordinary resource failure is still terminal. */
}


/* Test-only injector: never consume a failure on a no-op reservation.
 * Allocator failure testing must be deterministic and not need a real OOM.
 */
#ifdef ORESLANG_NATIVE_TEST_ALLOC_FAILURE
static _Atomic int64_t test_allocations_before_failure = -1;
void oreslang_int_array_test_fail_after(int64_t attempts) {
    if (attempts < -1) fail_closed();
    atomic_store_explicit(&test_allocations_before_failure, attempts, memory_order_relaxed);
}
static int test_injected_allocation_failure(void) {
    int64_t left = atomic_load_explicit(
        &test_allocations_before_failure, memory_order_relaxed);
    while (left >= 0) {
        if (left == 0) return 1;
        if (atomic_compare_exchange_weak_explicit(
                &test_allocations_before_failure, &left, left - 1,
                memory_order_relaxed, memory_order_relaxed))
            return 0;
    }
    return 0;
}
#else
static int test_injected_allocation_failure(void) { return 0; }
#endif

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

/* Only backing storage is counted: metadata wiping does not prove old
 * realloc buffers were scrubbed. This audit hook is test-only.
 */
#ifdef ORESLANG_NATIVE_TEST_ALLOC_FAILURE
static _Atomic uint64_t test_scrubbed_backing_bytes = 0;
void oreslang_int_array_test_reset_scrubbed_backing_bytes(void) {
    atomic_store_explicit(&test_scrubbed_backing_bytes, 0, memory_order_relaxed);
}
uint64_t oreslang_int_array_test_scrubbed_backing_bytes(void) {
    return atomic_load_explicit(&test_scrubbed_backing_bytes, memory_order_relaxed);
}
#endif

static void erase_backing_storage(int64_t* values, uint64_t capacity) {
    const size_t bytes = (size_t)capacity * sizeof(int64_t);
    erase_bytes(values, bytes);
#ifdef ORESLANG_NATIVE_TEST_ALLOC_FAILURE
    const volatile unsigned char* view = (const volatile unsigned char*)values;
    for (size_t i = 0; i < bytes; ++i)
        if (view[i] != 0) fail_closed();
    atomic_fetch_add_explicit(&test_scrubbed_backing_bytes, (uint64_t)bytes,
                              memory_order_relaxed);
#endif
}

/* The fast handle lookup validates its own slot; ownership-changing
 * operations additionally validate the *entire* region. Otherwise forged
 * aggregate counters can make close() free the region with live backing
 * buffers still allocated (and never zeroized), or make a new allocation
 * return a misleading ordinary quota status.
 *
 * Scan at create/reserve/drop/staging/close boundaries, not at each get/set,
 * to retain constant-time hot-path element access.
 */
static void validate_region_accounting(const OresNativeIntArrayRegion* region) {
    if (!region || region->live_count > MAX_ARRAY_SLOTS ||
            region->reserved_elements > REGION_ELEMENT_BUDGET) fail_closed();
    uint64_t live = 0;
    uint64_t reserved = 0;
    for (size_t i = 0; i < MAX_ARRAY_SLOTS; ++i) {
        const IntArraySlot* slot = &region->arrays[i];
        if (slot->token == 0) {
            if (slot->values || slot->capacity || slot->length)
                fail_closed(); /* No abandoned backing allocation. */
            continue;
        }
        if (slot->token == UINT64_MAX || slot->capacity > MAX_ARRAY_ELEMENTS ||
                slot->length > slot->capacity ||
                (slot->values == NULL) != (slot->capacity == 0))
            fail_closed();
        ++live;
        /* capacity is at most 65536, so the bounded sum cannot overflow. */
        reserved += slot->capacity;
        if (reserved > REGION_ELEMENT_BUDGET) fail_closed();
    }
    if (live != region->live_count || reserved != region->reserved_elements)
        fail_closed();
}

/* Check the entire invariant before *any* use, including a no-op
 * reservation, a read and an append that would otherwise index into values.
 * This validates internal trusted metadata, not a guest-supplied pointer.
 */
static IntArraySlot* require_slot(OresNativeIntArrayRegion* region,
                                  OresNativeIntArrayHandle handle) {
    if (!region || handle == 0 || handle == UINT64_MAX) fail_closed();
    if (region->live_count == 0 || region->live_count > MAX_ARRAY_SLOTS ||
            region->reserved_elements > REGION_ELEMENT_BUDGET) fail_closed();
    for (size_t i = 0; i < MAX_ARRAY_SLOTS; ++i) {
        IntArraySlot* slot = &region->arrays[i];
        if (slot->token == handle) {
            if (slot->length > slot->capacity ||
                    slot->capacity > MAX_ARRAY_ELEMENTS ||
                    slot->capacity > region->reserved_elements ||
                    (slot->values == NULL) != (slot->capacity == 0))
                fail_closed();
            return slot;
        }
    }
    fail_closed();
    return NULL;
}

#ifdef ORESLANG_NATIVE_TEST_ALLOC_FAILURE
/* Test-only invalid metadata injector: the real compiler cannot reach this. */
void oreslang_int_array_test_corrupt_length(
        OresNativeIntArrayRegion* region, OresNativeIntArrayHandle handle,
        uint64_t impossible_length) {
    require_slot(region, handle)->length = impossible_length;
}

/* Test-only corruption; never expose mutation of these private counters
 * to guest source or production builds.
 */
void oreslang_int_array_test_corrupt_region_accounting(
        OresNativeIntArrayRegion* region, uint64_t live, uint64_t reserved) {
    if (!region) fail_closed();
    region->live_count = live;
    region->reserved_elements = reserved;
}
#endif

OresNativeIntArrayStatus oreslang_int_array_try_region_open(
        OresNativeIntArrayRegion** out_region) {
    if (!out_region) fail_closed();
    if (test_injected_allocation_failure()) return ORES_NATIVE_ARRAY_NO_MEMORY;
    OresNativeIntArrayRegion* region = (OresNativeIntArrayRegion*)calloc(
            1, sizeof(OresNativeIntArrayRegion));
    if (!region) return ORES_NATIVE_ARRAY_NO_MEMORY;
    *out_region = region;
    return ORES_NATIVE_ARRAY_OK;
}

OresNativeIntArrayRegion* oreslang_int_array_region_open(void) {
    OresNativeIntArrayRegion* region = NULL;
    if (oreslang_int_array_try_region_open(&region) != ORES_NATIVE_ARRAY_OK)
        fail_closed();
    return region;
}

OresNativeIntArrayStatus oreslang_int_array_try_new(
        OresNativeIntArrayRegion* region, OresNativeIntArrayHandle* out_handle) {
    if (!region || !out_handle) fail_closed();
    validate_region_accounting(region);
    if (region->live_count == MAX_ARRAY_SLOTS) return ORES_NATIVE_ARRAY_LIMIT;
    for (size_t i = 0; i < MAX_ARRAY_SLOTS; ++i) {
        IntArraySlot* slot = &region->arrays[i];
        if (slot->token == 0) {
            if (slot->length || slot->capacity || slot->values)
                fail_closed(); /* Never reuse dirty/partially retired slots. */
            /* Logical slot acquisition counts as an allocation for the test
             * injector. On failure, metadata and out_handle stay untouched.
             */
            if (test_injected_allocation_failure()) return ORES_NATIVE_ARRAY_NO_MEMORY;
            const uint64_t token = allocate_token();
            slot->token = token;
            ++region->live_count;
            *out_handle = token;
            return ORES_NATIVE_ARRAY_OK;
        }
    }
    /* Inconsistent metadata is not ordinary quota exhaustion. */
    fail_closed();
    return ORES_NATIVE_ARRAY_LIMIT;
}

OresNativeIntArrayHandle oreslang_int_array_new(OresNativeIntArrayRegion* region) {
    OresNativeIntArrayHandle handle = 0;
    if (oreslang_int_array_try_new(region, &handle) != ORES_NATIVE_ARRAY_OK)
        fail_closed();
    return handle;
}

OresNativeIntArrayStatus oreslang_int_array_try_reserve(
        OresNativeIntArrayRegion* region,
        OresNativeIntArrayHandle handle, uint64_t capacity) {
    IntArraySlot* slot = require_slot(region, handle);
    validate_region_accounting(region);
    if (capacity > MAX_ARRAY_ELEMENTS || capacity > SIZE_MAX / sizeof(int64_t))
        return ORES_NATIVE_ARRAY_LIMIT;
    if (capacity <= slot->capacity) return ORES_NATIVE_ARRAY_OK;
    if (region->reserved_elements > REGION_ELEMENT_BUDGET) fail_closed();
    const uint64_t additional = capacity - slot->capacity;
    if (additional > REGION_ELEMENT_BUDGET - region->reserved_elements)
        return ORES_NATIVE_ARRAY_LIMIT;
    if (test_injected_allocation_failure()) return ORES_NATIVE_ARRAY_NO_MEMORY;

    /* realloc() may free a relocated buffer without clearing its former
     * contents. Allocate/copy/scrub/free explicitly so growth never leaves
     * live guest data in a discarded allocator block. calloc also keeps
     * unused slots zeroed, and failure leaves the old owner untouched.
     */
    int64_t* grown = (int64_t*)calloc((size_t)capacity, sizeof(int64_t));
    if (!grown) return ORES_NATIVE_ARRAY_NO_MEMORY;
    for (uint64_t i = 0; i < slot->length; ++i)
        grown[i] = slot->values[i];
    if (slot->values) {
        erase_backing_storage(slot->values, slot->capacity);
        free(slot->values);
    }
    region->reserved_elements += additional;
    slot->capacity = capacity;
    slot->values = grown;
    return ORES_NATIVE_ARRAY_OK;
}

void oreslang_int_array_reserve(OresNativeIntArrayRegion* region,
                               OresNativeIntArrayHandle handle, uint64_t capacity) {
    if (oreslang_int_array_try_reserve(region, handle, capacity) !=
            ORES_NATIVE_ARRAY_OK) fail_closed();
}

OresNativeIntArrayStatus oreslang_int_array_try_add(
        OresNativeIntArrayRegion* region,
        OresNativeIntArrayHandle handle, int64_t value) {
    IntArraySlot* slot = require_slot(region, handle);
    if (slot->length >= MAX_ARRAY_ELEMENTS) return ORES_NATIVE_ARRAY_LIMIT;
    if (slot->length == slot->capacity) {
        uint64_t capacity = slot->capacity == 0 ? 4 : slot->capacity * 2;
        if (capacity > MAX_ARRAY_ELEMENTS) capacity = MAX_ARRAY_ELEMENTS;
        OresNativeIntArrayStatus status =
            oreslang_int_array_try_reserve(region, handle, capacity);
        /* A geometric reservation can exceed the remaining regional quota
         * even when exactly one additional element still fits. Fall back to
         * the minimum append capacity, but only on LIMIT: NO_MEMORY remains
         * a recoverable, unchanged-state allocation failure. */
        if (status == ORES_NATIVE_ARRAY_LIMIT &&
                capacity > slot->length + 1)
            status = oreslang_int_array_try_reserve(
                region, handle, slot->length + 1);
        if (status != ORES_NATIVE_ARRAY_OK) return status;
    }
    slot->values[slot->length] = value;
    ++slot->length;
    return ORES_NATIVE_ARRAY_OK;
}

void oreslang_int_array_add(OresNativeIntArrayRegion* region,
                           OresNativeIntArrayHandle handle, int64_t value) {
    if (oreslang_int_array_try_add(region, handle, value) !=
            ORES_NATIVE_ARRAY_OK) fail_closed();
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
    validate_region_accounting(region);
    if (!region->live_count || slot->capacity > region->reserved_elements)
        fail_closed();
    if (slot->values) {
        erase_backing_storage(slot->values, slot->capacity);
        free(slot->values);
    }
    region->reserved_elements -= slot->capacity;
    --region->live_count;
    erase_bytes(slot, sizeof(*slot));
}

/* A prepared array is owned by the staging scope until all n succeed.
 * Failed preparation destroys every acquired handle, in reverse order, and
 * NEVER publishes partial results or retains reserved element capacity.
 */
OresNativeIntArrayStatus oreslang_int_array_try_prepare_many(
        OresNativeIntArrayRegion* region, uint64_t n,
        OresNativeIntArrayHandle* out) {
    if (!region || (n && !out)) fail_closed();
    validate_region_accounting(region);
    if (n > MAX_ARRAY_SLOTS ||
            n > MAX_ARRAY_SLOTS - region->live_count)
        return ORES_NATIVE_ARRAY_LIMIT;
    OresNativeIntArrayHandle staged[MAX_ARRAY_SLOTS] = {0};
    uint64_t initialized = 0;
    for (; initialized < n; ++initialized) {
        OresNativeIntArrayStatus status =
            oreslang_int_array_try_new(region, &staged[initialized]);
        if (status != ORES_NATIVE_ARRAY_OK) {
            while (initialized) {
                --initialized;
                oreslang_int_array_drop(region, staged[initialized]);
            }
            return status;
        }
    }
    for (uint64_t i = 0; i < n; ++i) out[i] = staged[i];
    return ORES_NATIVE_ARRAY_OK;
}

/* Fill all staged owners in isolation. This operation creates the four
 * initialized backing arrays needed by the linked queue without allowing
 * any intermediate class-field publication. Every memory/slot allocation
 * edge is fallible and every abort-free failure path rolls back all owners.
 */
OresNativeIntArrayStatus oreslang_int_array_try_prepare_filled_many(
        OresNativeIntArrayRegion* region, uint64_t n,
        const uint64_t lengths[], const int64_t values[],
        OresNativeIntArrayHandle out[]) {
    if (!region || (n && (!lengths || !values || !out))) fail_closed();
    validate_region_accounting(region);
    if (n > MAX_ARRAY_SLOTS - region->live_count)
        return ORES_NATIVE_ARRAY_LIMIT;

    /* Perform the complete input and quota validation before requesting a
     * token or a byte. This also avoids arithmetic overflow in aggregate
     * length accounting, because remaining_budget is always bounded.
     */
    uint64_t remaining_budget = REGION_ELEMENT_BUDGET - region->reserved_elements;
    for (uint64_t i = 0; i < n; ++i) {
        if (lengths[i] > MAX_ARRAY_ELEMENTS ||
                lengths[i] > SIZE_MAX / sizeof(int64_t) ||
                lengths[i] > remaining_budget)
            return ORES_NATIVE_ARRAY_LIMIT;
        remaining_budget -= lengths[i];
    }

    OresNativeIntArrayHandle staged[MAX_ARRAY_SLOTS] = {0};
    uint64_t acquired = 0;
    OresNativeIntArrayStatus status = ORES_NATIVE_ARRAY_OK;
    for (uint64_t i = 0; i < n; ++i) {
        status = oreslang_int_array_try_new(region, &staged[i]);
        if (status != ORES_NATIVE_ARRAY_OK) goto rollback;
        ++acquired;
        if (lengths[i] == 0) continue;
        status = oreslang_int_array_try_reserve(region, staged[i], lengths[i]);
        if (status != ORES_NATIVE_ARRAY_OK) goto rollback;
        IntArraySlot* slot = require_slot(region, staged[i]);
        for (uint64_t j = 0; j < lengths[i]; ++j)
            slot->values[j] = values[i];
        slot->length = lengths[i];
    }
    for (uint64_t i = 0; i < n; ++i) out[i] = staged[i];
    return ORES_NATIVE_ARRAY_OK;

rollback:
    while (acquired != 0) {
        --acquired;
        oreslang_int_array_drop(region, staged[acquired]);
    }
    return status;
}

/* Once validation succeeds, commit makes no fallible allocation. Every
 * destination and staged source is unique, and no old destination is also a
 * staged source. Invalid handles are never treated as resource failures.
 */
void oreslang_int_array_commit_many(
        OresNativeIntArrayRegion* region, uint64_t n,
        OresNativeIntArrayHandle* const destinations[],
        OresNativeIntArrayHandle* const sources[]) {
    if (!region || n > MAX_ARRAY_SLOTS ||
            (n && (!destinations || !sources))) fail_closed();
    OresNativeIntArrayHandle old[MAX_ARRAY_SLOTS] = {0};
    OresNativeIntArrayHandle next[MAX_ARRAY_SLOTS] = {0};
    for (uint64_t i = 0; i < n; ++i) {
        if (!destinations[i] || !sources[i] || destinations[i] == sources[i])
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
                    (old[i] && old[i] == old[j])) fail_closed();
        }
    }
    for (uint64_t i = 0; i < n; ++i) {
        for (uint64_t j = 0; j < n; ++j) {
            if (old[i] && old[i] == next[j]) fail_closed();
        }
    }
    for (uint64_t i = 0; i < n; ++i)
        if (old[i]) oreslang_int_array_drop(region, old[i]);
    for (uint64_t i = 0; i < n; ++i) {
        *destinations[i] = next[i];
        *sources[i] = 0;
    }
}

void oreslang_int_array_region_close(OresNativeIntArrayRegion* region) {
    validate_region_accounting(region);
    if (region->live_count || region->reserved_elements)
        fail_closed();
    erase_bytes(region, sizeof(*region));
    free(region);
}
