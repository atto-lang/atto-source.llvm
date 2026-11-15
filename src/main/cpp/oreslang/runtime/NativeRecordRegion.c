#include "NativeRecordRegion.h"
#include <limits.h>
#include <stddef.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum { RECORD_LIMIT = 65536, FIELD_LIMIT = 16 };
typedef struct { int64_t schema; size_t count; uint64_t writable_fields; int64_t fields[FIELD_LIMIT]; } Record;
typedef struct {
    Record* records;
    size_t count, capacity;
    uint32_t generation;
    int active, failed;
} Region;
static _Thread_local Region region;
static _Atomic uint32_t next_generation = 1;
#ifdef ORESLANG_RECORD_TEST_ALLOC_FAILURE
static _Thread_local int64_t fail_after = -1;
void ores_record_test_fail_after(int64_t attempts) { fail_after = attempts; }
void ores_record_test_exhaust_generation(void) { atomic_store(&next_generation, INT32_MAX); }
static int allocation_allowed(void) {
    if (fail_after < 0) return 1;
    if (fail_after == 0) return 0;
    --fail_after;
    return 1;
}
#else
static int allocation_allowed(void) { return 1; }
#endif

int64_t ores_record_region_begin(void) {
    if (region.active) { region.failed = 1; return 1; }
    uint32_t generation = atomic_load_explicit(&next_generation, memory_order_relaxed);
    for (;;) {
        /* Signed guest handles stay positive. Exhaustion never recycles. */
        if (generation >= INT32_MAX) return 1;
        if (atomic_compare_exchange_weak_explicit(&next_generation, &generation,
                generation + 1, memory_order_relaxed, memory_order_relaxed)) break;
    }
    memset(&region, 0, sizeof region);
    region.generation = generation;
    region.active = 1;
    return 0;
}

int64_t ores_record_new_mutable(int64_t schema, int64_t fields, const int64_t* values,
                              uint64_t writable_fields) {
    Region* r = &region;
    if (!r->active || r->failed || schema <= 0 || fields < 0 || fields > FIELD_LIMIT ||
            (fields != 0 && !values) || r->count >= RECORD_LIMIT ||
            (fields >= 0 && fields <= FIELD_LIMIT && (writable_fields >> (unsigned)fields) != 0)) {
        r->failed = 1;
        return 0;
    }
    if (r->count == r->capacity) {
        size_t capacity = r->capacity ? r->capacity * 2 : 32;
        if (capacity > RECORD_LIMIT) capacity = RECORD_LIMIT;
        if (capacity > SIZE_MAX / sizeof(Record) || !allocation_allowed()) {
            r->failed = 1;
            return 0;
        }
        Record* records = realloc(r->records, capacity * sizeof(Record));
        if (!records) { r->failed = 1; return 0; }
        r->records = records;
        r->capacity = capacity;
    }
    Record* record = &r->records[r->count];
    memset(record, 0, sizeof *record);
    record->schema = schema;
    record->count = (size_t)fields;
    record->writable_fields = writable_fields;
    if (fields) memcpy(record->fields, values, (size_t)fields * sizeof(int64_t));
    /* Publication follows complete initialization; realloc never changes
     * logical identities and no payload pointer leaves the region. */
    ++r->count;
    return (int64_t)(((uint64_t)r->generation << 32) | r->count);
}

int64_t ores_record_new(int64_t schema, int64_t fields, const int64_t* values) {
    return ores_record_new_mutable(schema, fields, values, 0);
}

static const Record* lookup(int64_t handle, int64_t schema) {
    Region* r = &region;
    const uint64_t token = (uint64_t)handle;
    const uint32_t slot = (uint32_t)token;
    if (!r->active || r->failed || handle <= 0 || schema <= 0 ||
            (uint32_t)(token >> 32) != r->generation || slot == 0 || slot > r->count) {
        r->failed = 1;
        return NULL;
    }
    const Record* record = &r->records[slot - 1];
    if (record->schema != schema) {
        r->failed = 1;
        return NULL;
    }
    return record;
}

int64_t ores_record_check_shape(int64_t handle, int64_t schema, int64_t fields) {
    if (fields < 0 || fields > FIELD_LIMIT) { region.failed = 1; return 1; }
    const Record* record = lookup(handle, schema);
    if (!record || record->count != (uint64_t)fields) { region.failed = 1; return 1; }
    return 0;
}

int64_t ores_record_check(int64_t handle, int64_t schema) {
    return lookup(handle, schema) ? 0 : 1;
}

int64_t ores_record_set(int64_t handle, int64_t schema, int64_t field, int64_t value) {
    const Record* record = lookup(handle, schema);
    if (!record || field < 0 || (uint64_t)field >= record->count ||
            !(record->writable_fields & (UINT64_C(1) << (unsigned)field))) {
        region.failed = 1;
        return 1;
    }
    /* Resolve by identity after every possible growth; never cache a pointer. */
    region.records[(uint32_t)(uint64_t)handle - 1].fields[field] = value;
    return 0;
}

int64_t ores_record_get(int64_t handle, int64_t schema, int64_t field) {
    const Record* record = lookup(handle, schema);
    if (!record) return 0;
    if (field < 0 || (uint64_t)field >= record->count) {
        region.failed = 1;
        return 0;
    }
    return record->fields[field];
}

int64_t ores_record_region_end(void) {
    Region* r = &region;
    if (!r->active) return 1;
    const int64_t result = r->failed ? 1 : 0;
    volatile unsigned char* bytes = (volatile unsigned char*)r->records;
    for (size_t i = 0; i < r->capacity * sizeof(Record); ++i) bytes[i] = 0;
    free(r->records);
    memset(r, 0, sizeof *r);
    return result;
}
