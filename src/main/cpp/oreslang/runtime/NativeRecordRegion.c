#include "NativeRecordRegion.h"
#include <limits.h>
#include <stddef.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Compact region storage. Records used to be a fixed 152-byte struct (16
 * int64 slots whatever the real field count). They are now packed in a bump
 * arena at their exact size: a 16-byte header plus 8 bytes per actual field.
 * A 32-bit offset index maps each logical slot to its record. Records are
 * never freed one by one (the whole region ends at once), so a bump arena is
 * the optimal layout. Guest handles are unchanged: (generation << 32) | slot.
 * No payload address ever escapes, and growth only moves the arena, never a
 * logical identity. */
enum { RECORD_LIMIT = 65536, FIELD_LIMIT = 16 };
typedef struct {
    int64_t schema;
    uint32_t count;           /* fields actually stored */
    uint32_t writable_fields; /* bit i: field i writable (FIELD_LIMIT <= 32) */
} RecordHeader;
_Static_assert(sizeof(RecordHeader) == 16, "record header must stay 16 bytes");
_Static_assert(FIELD_LIMIT <= 32, "writable mask must fit 32 bits");
/* Largest possible arena: every record at FIELD_LIMIT fields. */
#define ARENA_LIMIT ((size_t)RECORD_LIMIT * (sizeof(RecordHeader) + FIELD_LIMIT * sizeof(int64_t)))
_Static_assert(ARENA_LIMIT <= UINT32_MAX, "record offsets must fit 32 bits");
enum { INITIAL_INDEX = 32, INITIAL_ARENA = 32 * (16 + 2 * 8) };

typedef struct {
    unsigned char* arena;     /* packed records, 8-byte aligned */
    size_t used, arena_capacity;
    uint32_t* index;          /* slot - 1 -> byte offset in arena */
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

/* One growth event (one allocation attempt) covers both arrays, so the
 * failure-injection hook counts growth exactly as before. Failure leaves the
 * region unchanged apart from marking it failed. */
static int grow(Region* r, size_t need_bytes) {
    size_t capacity = r->capacity;
    if (r->count == capacity) {
        capacity = capacity ? capacity * 2 : INITIAL_INDEX;
        if (capacity > RECORD_LIMIT) capacity = RECORD_LIMIT;
    }
    size_t arena_capacity = r->arena_capacity;
    if (r->arena_capacity - r->used < need_bytes) {
        arena_capacity = arena_capacity ? arena_capacity * 2 : INITIAL_ARENA;
        while (arena_capacity - r->used < need_bytes) arena_capacity *= 2;
        if (arena_capacity > ARENA_LIMIT) arena_capacity = ARENA_LIMIT;
    }
    if (!allocation_allowed()) return 0;
    if (capacity != r->capacity) {
        uint32_t* index = realloc(r->index, capacity * sizeof(uint32_t));
        if (!index) return 0;
        r->index = index;
        r->capacity = capacity;
    }
    if (arena_capacity != r->arena_capacity) {
        unsigned char* arena = realloc(r->arena, arena_capacity);
        if (!arena) return 0;
        r->arena = arena;
        r->arena_capacity = arena_capacity;
    }
    return 1;
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
    const size_t bytes = sizeof(RecordHeader) + (size_t)fields * sizeof(int64_t);
    if ((r->count == r->capacity || r->arena_capacity - r->used < bytes) && !grow(r, bytes)) {
        r->failed = 1;
        return 0;
    }
    RecordHeader header = {schema, (uint32_t)fields, (uint32_t)writable_fields};
    memcpy(r->arena + r->used, &header, sizeof header);
    if (fields) memcpy(r->arena + r->used + sizeof header, values, (size_t)fields * sizeof(int64_t));
    /* Publication follows complete initialization. */
    r->index[r->count] = (uint32_t)r->used;
    r->used += bytes;
    ++r->count;
    return (int64_t)(((uint64_t)r->generation << 32) | r->count);
}

int64_t ores_record_new(int64_t schema, int64_t fields, const int64_t* values) {
    return ores_record_new_mutable(schema, fields, values, 0);
}

/* Validate a handle; on success copy out the header and return the byte offset
 * of field 0. Never returns or caches a pointer: growth may move the arena. */
static int lookup(int64_t handle, int64_t schema, RecordHeader* header, size_t* fields_at) {
    Region* r = &region;
    const uint64_t token = (uint64_t)handle;
    const uint32_t slot = (uint32_t)token;
    if (!r->active || r->failed || handle <= 0 || schema <= 0 ||
            (uint32_t)(token >> 32) != r->generation || slot == 0 || slot > r->count) {
        r->failed = 1;
        return 0;
    }
    const size_t at = r->index[slot - 1];
    memcpy(header, r->arena + at, sizeof *header);
    if (header->schema != schema) {
        r->failed = 1;
        return 0;
    }
    *fields_at = at + sizeof(RecordHeader);
    return 1;
}

int64_t ores_record_check_shape(int64_t handle, int64_t schema, int64_t fields) {
    if (fields < 0 || fields > FIELD_LIMIT) { region.failed = 1; return 1; }
    RecordHeader header;
    size_t at;
    if (!lookup(handle, schema, &header, &at) || header.count != (uint64_t)fields) {
        region.failed = 1;
        return 1;
    }
    return 0;
}

int64_t ores_record_check(int64_t handle, int64_t schema) {
    RecordHeader header;
    size_t at;
    return lookup(handle, schema, &header, &at) ? 0 : 1;
}

int64_t ores_record_set(int64_t handle, int64_t schema, int64_t field, int64_t value) {
    RecordHeader header;
    size_t at;
    if (!lookup(handle, schema, &header, &at) || field < 0 || (uint64_t)field >= header.count ||
            !(header.writable_fields & (UINT32_C(1) << (unsigned)field))) {
        region.failed = 1;
        return 1;
    }
    memcpy(region.arena + at + (size_t)field * sizeof(int64_t), &value, sizeof value);
    return 0;
}

int64_t ores_record_get(int64_t handle, int64_t schema, int64_t field) {
    RecordHeader header;
    size_t at;
    if (!lookup(handle, schema, &header, &at)) return 0;
    if (field < 0 || (uint64_t)field >= header.count) {
        region.failed = 1;
        return 0;
    }
    int64_t value;
    memcpy(&value, region.arena + at + (size_t)field * sizeof(int64_t), sizeof value);
    return value;
}

int64_t ores_record_region_end(void) {
    Region* r = &region;
    if (!r->active) return 1;
    const int64_t result = r->failed ? 1 : 0;
    volatile unsigned char* bytes = (volatile unsigned char*)r->arena;
    for (size_t i = 0; i < r->arena_capacity; ++i) bytes[i] = 0;
    volatile unsigned char* slots = (volatile unsigned char*)r->index;
    for (size_t i = 0; i < r->capacity * sizeof(uint32_t); ++i) slots[i] = 0;
    free(r->arena);
    free(r->index);
    memset(r, 0, sizeof *r);
    return result;
}
