#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* No tracing GC, no reference counting, and no cross-actor ownership.
 * This is a native lexical ownership domain: region lifetime is one
 * compiled function activation until actor-lifetime integration lands. */
typedef struct OresOwnedI64Region OresOwnedI64Region;

OresOwnedI64Region* oreslang_owned_i64_region_open(uint64_t slots);
int64_t* oreslang_owned_i64_region_new(OresOwnedI64Region* region, int64_t value);
void oreslang_owned_i64_region_drop(OresOwnedI64Region* region, int64_t* slot);
void oreslang_owned_i64_region_close(OresOwnedI64Region* region);

/* Generation-checked by-value handle ABI for compiler-generated owners.
 * Never expose a slot's raw address; handles become invalid after drop and
 * cannot acquire a later generation at the same slot. */
OresOwnedI64Region* oreslang_owned_i64_region_open_handles(uint64_t slots);
uint64_t oreslang_owned_i64_region_new_handle(OresOwnedI64Region* region, int64_t value);
int64_t oreslang_owned_i64_region_read_handle(OresOwnedI64Region* region, uint64_t handle);
void oreslang_owned_i64_region_drop_handle(OresOwnedI64Region* region, uint64_t handle);

#ifdef __cplusplus
}
#endif
