#pragma once
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/*
 * Trusted, caller-owned, allocation-free Map<int,int> substrate for the LLVM
 * backend. This is a runtime representation building block, NOT source-level
 * structural Map parity yet.
 *
 * The compiler owns both the region and its fixed-capacity entry storage.
 * No entry pointer escapes, no operation allocates, no deletion/recycling
 * occurs, and iteration is deterministic insertion order. Missing lookup is
 * represented explicitly through the boolean return value; it never invents
 * a required key. Invalid metadata/pointers fail closed by aborting.
 */
typedef struct {
    int64_t key;
    int64_t value;
    uint8_t occupied;
    uint8_t reserved[7];
} OresNativeI64MapEntry;

typedef struct {
    uint64_t magic;
    uint64_t capacity;
    uint64_t size;
    OresNativeI64MapEntry* entries;
} OresNativeI64MapRegion;

typedef enum {
    ORES_NATIVE_I64_MAP_OK = 0,
    ORES_NATIVE_I64_MAP_FULL = 1
} OresNativeI64MapStatus;

/*
 * Compiler-owned fixed-capacity ABI. Capacity is selected before construction,
 * must be in [1, ORES_NATIVE_I64_MAP_MAX_CAPACITY], and never grows implicitly.
 * A full map reports ORES_NATIVE_I64_MAP_FULL without mutating existing state.
 */
enum { ORES_NATIVE_I64_MAP_MAX_CAPACITY = 4096 };

size_t oreslang_i64_map_storage_bytes(uint64_t capacity);
size_t oreslang_i64_map_storage_alignment(void);
size_t oreslang_i64_map_region_alignment(void);

void oreslang_i64_map_init(
    OresNativeI64MapRegion* region,
    OresNativeI64MapEntry* storage,
    uint64_t capacity);

OresNativeI64MapStatus oreslang_i64_map_put(
    OresNativeI64MapRegion* region,
    int64_t key,
    int64_t value);

/* Returns 1 when present and writes *out_value, 0 when absent.
 * Missing lookup leaves *out_value unchanged.
 */
int oreslang_i64_map_get(
    const OresNativeI64MapRegion* region,
    int64_t key,
    int64_t* out_value);

uint64_t oreslang_i64_map_size(const OresNativeI64MapRegion* region);

/* Deterministic insertion-order iteration. Returns 1 for a valid ordinal,
 * 0 when ordinal >= size. Out parameters remain unchanged on the latter.
 */
int oreslang_i64_map_entry_at(
    const OresNativeI64MapRegion* region,
    uint64_t ordinal,
    int64_t* out_key,
    int64_t* out_value);

/* Zeroizes every entry and disarms the region. The caller retains storage. */
void oreslang_i64_map_close(OresNativeI64MapRegion* region);

#ifdef __cplusplus
}
#endif
