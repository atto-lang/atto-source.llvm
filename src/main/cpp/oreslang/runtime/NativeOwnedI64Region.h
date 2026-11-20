#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Trusted compiler/host-only synchronous ownership ABI.
 * The monotonically unique token prevents slot/region reuse ABA within this
 * process. It is NOT an unforgeable capability, tenant authorization token,
 * cross-process identity, or a concurrency-safe reference. A caller must
 * already own the region and may only use it while that region is live on
 * its serialized owning turn. Invalid tokens abort the native process;
 * never expose this low-level ABI to arbitrary untrusted/remote inputs. */
typedef struct OresOwnedI64Region OresOwnedI64Region;
typedef uint64_t OresOwnedI64Handle;
OresOwnedI64Region* oreslang_owned_i64_region_open(uint64_t slots);
OresOwnedI64Handle oreslang_owned_i64_region_new(OresOwnedI64Region* region, int64_t value);
int64_t oreslang_owned_i64_region_read(OresOwnedI64Region* region, OresOwnedI64Handle handle);
void oreslang_owned_i64_region_drop(OresOwnedI64Region* region, OresOwnedI64Handle handle);
void oreslang_owned_i64_region_close(OresOwnedI64Region* region);
#ifdef ORESLANG_NATIVE_TEST_TOKEN_EXHAUSTION
void oreslang_owned_i64_test_set_next_token(uint64_t next);
#endif
#ifdef __cplusplus
}
#endif
