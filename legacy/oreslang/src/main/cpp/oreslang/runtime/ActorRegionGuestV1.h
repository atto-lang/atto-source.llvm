#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/*
 * Restricted, synchronous guest bridge for compiler-generated code.
 *
 * The guest receives only a 32-bit region-relative offset. OresArenaTurn and
 * OresActorRegionV1Ref (domain/index/generation) stay in a trusted TLS
 * sidecar. This is NOT a pointer, a standalone capability, a scheduler actor
 * integration, or a sandbox boundary. Only a sealed compiler profile that
 * forbids FFI, raw integer casts, suspension and concurrency may emit calls.
 *
 * The root native entry wrapper opens/closes this thread-affine scope. The
 * host bridge mints actor identity; the source program cannot supply it.
 */
void ores_actor_region_guest_v1_begin(uint32_t object_slots,
                                      uint32_t slot_bytes,
                                      uint32_t scratch_bytes);
uint32_t ores_actor_region_guest_v1_alloc(uint32_t object_bytes);
int64_t ores_actor_region_guest_v1_read_i64(uint32_t offset,
                                            uint32_t field_offset);
void ores_actor_region_guest_v1_write_i64(uint32_t offset,
                                          uint32_t field_offset,
                                          int64_t value);
void ores_actor_region_guest_v1_drop(uint32_t offset);
/* Fails closed if compiler-generated cleanup left any object live. */
void ores_actor_region_guest_v1_end(void);
#ifdef __cplusplus
}
#endif
