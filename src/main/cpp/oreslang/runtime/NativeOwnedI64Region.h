#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* Lexical native ownership domain. Call close only after all operations on
 * that region have quiesced; concurrent close/use is not a valid lifetime.
 * No raw payload pointer is returned or accepted by this public ABI. */
typedef struct OresOwnedI64Region OresOwnedI64Region;

/* Handles are lifetime-unique within the process; they are NOT transferable
 * ownership capabilities. Only the owning region may access/drop a handle. */
OresOwnedI64Region* oreslang_owned_i64_region_open_handles(uint64_t slots);
uint64_t oreslang_owned_i64_region_new_handle(OresOwnedI64Region* region, int64_t value);
int64_t oreslang_owned_i64_region_read_handle(OresOwnedI64Region* region, uint64_t handle);
void oreslang_owned_i64_region_drop_handle(OresOwnedI64Region* region, uint64_t handle);
void oreslang_owned_i64_region_close(OresOwnedI64Region* region);

#ifdef __cplusplus
}
#endif
