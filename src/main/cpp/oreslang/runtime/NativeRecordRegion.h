#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Compiler-only scalar record storage (immutable by default, or a checked
 * writable-field mask for mutable scalar collection elements).
 * Handles carry invocation generation
 * and slot index, never an address. The synchronous root owns all records;
 * class/array views cannot outlive it. No tracing collector, per-record free,
 * actor transfer or async lifetime is implied by this ABI. */
int64_t ores_record_region_begin(void);
int64_t ores_record_region_end(void);
int64_t ores_record_new(int64_t schema, int64_t fields, const int64_t* values);
/* Mutable scalar records use the same stable logical identity. A compiler
 * schema fixes writable field bits; immutable records always have mask zero.
 * No payload address escapes, including when the arena grows. All accesses
 * remain confined to the serialized synchronous invocation owner. */
int64_t ores_record_new_mutable(int64_t schema, int64_t fields,
    const int64_t* values, uint64_t writable_fields);
int64_t ores_record_set(int64_t handle, int64_t schema, int64_t field, int64_t value);
int64_t ores_record_get(int64_t handle, int64_t schema, int64_t field);
int64_t ores_record_check(int64_t handle, int64_t schema);
/* Validate exact nominal shape before inspecting a tagged sum payload. */
int64_t ores_record_check_shape(int64_t handle, int64_t schema, int64_t fields);
#ifdef ORESLANG_RECORD_TEST_ALLOC_FAILURE
void ores_record_test_fail_after(int64_t attempts);
void ores_record_test_exhaust_generation(void);
#endif
#ifdef __cplusplus
}
#endif
