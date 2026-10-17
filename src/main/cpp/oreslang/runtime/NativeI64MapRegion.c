#include "oreslang/runtime/NativeI64MapRegion.h"
#include <stddef.h>
#include <stdlib.h>

_Static_assert(sizeof(OresNativeI64MapEntry) == 24,
    "native i64 map entry ABI must remain 24 bytes");
_Static_assert(offsetof(OresNativeI64MapEntry, key) == 0,
    "native i64 map key ABI drift");
_Static_assert(offsetof(OresNativeI64MapEntry, value) == 8,
    "native i64 map value ABI drift");
_Static_assert(offsetof(OresNativeI64MapEntry, occupied) == 16,
    "native i64 map occupancy ABI drift");

enum { ORES_NATIVE_I64_MAP_MAX_CAPACITY = 4096 };
static const uint64_t ORES_NATIVE_I64_MAP_MAGIC = UINT64_C(0x4f52454d41504936);

static void fail_closed(void) { abort(); }

static int ranges_overlap(
        const void* left,
        size_t left_size,
        const void* right,
        size_t right_size) {
    const uintptr_t left_start = (uintptr_t)left;
    const uintptr_t right_start = (uintptr_t)right;
    if (left_size > UINTPTR_MAX - left_start ||
            right_size > UINTPTR_MAX - right_start)
        fail_closed();
    const uintptr_t left_end = left_start + left_size;
    const uintptr_t right_end = right_start + right_size;
    return left_start < right_end && right_start < left_end;
}

static void validate_region_storage_disjoint(
        const OresNativeI64MapRegion* region,
        const OresNativeI64MapEntry* storage,
        uint64_t capacity) {
    const size_t storage_bytes =
        (size_t)capacity * sizeof(OresNativeI64MapEntry);
    if (ranges_overlap(region, sizeof(*region), storage, storage_bytes))
        fail_closed();
}

static void validate_output_pointer(
        const OresNativeI64MapRegion* region,
        const void* output,
        size_t output_size) {
    if (!output) fail_closed();
    if (ranges_overlap(output, output_size, region, sizeof(*region)))
        fail_closed();
    const size_t storage_bytes =
        (size_t)region->capacity * sizeof(OresNativeI64MapEntry);
    if (ranges_overlap(output, output_size, region->entries, storage_bytes))
        fail_closed();
}

static void erase_bytes(void* data, size_t count) {
    volatile unsigned char* bytes = (volatile unsigned char*)data;
    for (size_t i = 0; i < count; ++i) bytes[i] = 0;
}

static void validate(const OresNativeI64MapRegion* region) {
    if (!region || region->magic != ORES_NATIVE_I64_MAP_MAGIC ||
            !region->entries || region->capacity == 0 ||
            region->capacity > ORES_NATIVE_I64_MAP_MAX_CAPACITY ||
            region->size > region->capacity)
        fail_closed();
    validate_region_storage_disjoint(
        region, region->entries, region->capacity);

    /*
     * No deletion/recycling exists, so valid storage is a compact occupied
     * prefix [0,size) followed by byte-clean empty slots [size,capacity).
     * Enforcing that invariant catches torn/corrupted occupancy metadata and
     * prevents hidden stale key/value bytes from becoming future entries.
     */
    for (uint64_t i = 0; i < region->capacity; ++i) {
        const OresNativeI64MapEntry* entry = &region->entries[i];
        if (entry->occupied > 1) fail_closed();
        for (size_t j = 0; j < sizeof(entry->reserved); ++j)
            if (entry->reserved[j] != 0) fail_closed();

        if (i < region->size) {
            if (!entry->occupied) fail_closed();
        } else {
            if (entry->occupied || entry->key != 0 || entry->value != 0)
                fail_closed();
        }
    }
}

void oreslang_i64_map_init(
        OresNativeI64MapRegion* region,
        OresNativeI64MapEntry* storage,
        uint64_t capacity) {
    if (!region || !storage || capacity == 0 ||
            capacity > ORES_NATIVE_I64_MAP_MAX_CAPACITY)
        fail_closed();
    validate_region_storage_disjoint(region, storage, capacity);

    for (uint64_t i = 0; i < capacity; ++i)
        erase_bytes(&storage[i], sizeof(storage[i]));
    region->capacity = capacity;
    region->size = 0;
    region->entries = storage;
    region->magic = ORES_NATIVE_I64_MAP_MAGIC;
    validate(region);
}

OresNativeI64MapStatus oreslang_i64_map_put(
        OresNativeI64MapRegion* region,
        int64_t key,
        int64_t value) {
    validate(region);

    OresNativeI64MapEntry* matched = NULL;
    for (uint64_t i = 0; i < region->capacity; ++i) {
        OresNativeI64MapEntry* entry = &region->entries[i];
        if (!entry->occupied || entry->key != key) continue;
        if (matched) fail_closed(); /* Corrupted duplicate map key. */
        matched = entry;
    }
    if (matched) {
        matched->value = value;
        return ORES_NATIVE_I64_MAP_OK;
    }

    if (region->size == region->capacity)
        return ORES_NATIVE_I64_MAP_FULL;

    for (uint64_t i = 0; i < region->capacity; ++i) {
        OresNativeI64MapEntry* entry = &region->entries[i];
        if (!entry->occupied) {
            entry->key = key;
            entry->value = value;
            entry->occupied = 1;
            ++region->size;
            validate(region);
            return ORES_NATIVE_I64_MAP_OK;
        }
    }
    fail_closed();
    return ORES_NATIVE_I64_MAP_FULL;
}

int oreslang_i64_map_get(
        const OresNativeI64MapRegion* region,
        int64_t key,
        int64_t* out_value) {
    if (!out_value) fail_closed();
    validate(region);
    validate_output_pointer(region, out_value, sizeof(*out_value));
    const OresNativeI64MapEntry* matched = NULL;
    for (uint64_t i = 0; i < region->capacity; ++i) {
        const OresNativeI64MapEntry* entry = &region->entries[i];
        if (!entry->occupied || entry->key != key) continue;
        if (matched) fail_closed(); /* Ambiguous lookup cannot be tolerated. */
        matched = entry;
    }
    if (!matched) return 0;
    *out_value = matched->value;
    return 1;
}

uint64_t oreslang_i64_map_size(const OresNativeI64MapRegion* region) {
    validate(region);
    return region->size;
}

int oreslang_i64_map_entry_at(
        const OresNativeI64MapRegion* region,
        uint64_t ordinal,
        int64_t* out_key,
        int64_t* out_value) {
    if (!out_key || !out_value) fail_closed();
    validate(region);
    validate_output_pointer(region, out_key, sizeof(*out_key));
    validate_output_pointer(region, out_value, sizeof(*out_value));
    if (ranges_overlap(out_key, sizeof(*out_key), out_value, sizeof(*out_value)))
        fail_closed();
    if (ordinal >= region->size) return 0;
    uint64_t seen = 0;
    for (uint64_t i = 0; i < region->capacity; ++i) {
        const OresNativeI64MapEntry* entry = &region->entries[i];
        if (!entry->occupied) continue;
        if (seen++ == ordinal) {
            for (uint64_t j = 0; j < region->size; ++j) {
                if (j != i && region->entries[j].key == entry->key)
                    fail_closed(); /* Iteration cannot expose duplicate keys. */
            }
            *out_key = entry->key;
            *out_value = entry->value;
            return 1;
        }
    }
    fail_closed();
    return 0;
}

void oreslang_i64_map_close(OresNativeI64MapRegion* region) {
    validate(region);
    OresNativeI64MapEntry* entries = region->entries;
    const uint64_t capacity = region->capacity;
    for (uint64_t i = 0; i < capacity; ++i)
        erase_bytes(&entries[i], sizeof(entries[i]));
    erase_bytes(region, sizeof(*region));
}
