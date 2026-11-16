#pragma once
#include "oreslang/runtime/ActorArena.h"
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* Trusted host ABI for bounded actor-owned byte objects. Not exposed to
 * untrusted guest source as a raw pointer. This is an additive bridge over
 * ActorArena, not an alternative allocator or complete compiler lowering.
 * A 32-bit encoded offset contains NO lifetime, tenant or access authority;
 * it must travel with the ORIGINAL out-of-line generation/domain proof. */
typedef struct OresActorRegionV1 OresActorRegionV1;
typedef struct { uint32_t storage; } OresActorOffset32;
typedef struct {
    OresActorOffset32 offset;
    OresArenaHandle proof;
} OresActorRegionV1Ref;
typedef struct {
    OresActorRegionV1Ref ref;
    OresArenaLoan proof;
} OresActorRegionV1Loan;

/* Same physical security boundary as ActorArena. Startup reserves all memory;
 * hot-path operations do not require managed allocations or tracing GC.
 * Allowed: 1..4096 objects, 1..1MiB each, total arena <=64MiB including
 * scratch. Invalid configuration and illegal guest access fail-stop. */
OresActorRegionV1* ores_actor_region_v1_open(uint64_t actor_id,
    uint64_t incarnation, uint32_t object_slots, uint32_t slot_bytes,
    uint32_t scratch_bytes);
int ores_actor_region_v1_prefault(OresActorRegionV1*);
OresArenaTurn ores_actor_region_v1_enter(OresActorRegionV1*,
    uint64_t actor_id, uint64_t incarnation);
OresArenaTurn ores_actor_region_v1_enter_realtime(OresActorRegionV1*,
    uint64_t actor_id, uint64_t incarnation);
void ores_actor_region_v1_leave(OresActorRegionV1*, OresArenaTurn);

/* offset.storage == 1 + physical slot byte offset. Zero is null.
 * NO API reconstructs a proof from a bare compact offset; this prevents ABA
 * when an actor drops and reuses the same physical byte location. */
OresActorRegionV1Ref ores_actor_region_v1_alloc(OresActorRegionV1*,
    OresArenaTurn, uint32_t object_bytes);
void ores_actor_region_v1_read(OresActorRegionV1*, OresArenaTurn,
    OresActorRegionV1Ref, uint32_t field_offset, void*, uint32_t bytes);
void ores_actor_region_v1_write(OresActorRegionV1*, OresArenaTurn,
    OresActorRegionV1Ref, uint32_t field_offset, const void*, uint32_t bytes);
void ores_actor_region_v1_drop(OresActorRegionV1*, OresArenaTurn,
    OresActorRegionV1Ref*);

/* Shared read leases survive mailbox turns and prohibit mutation and drop.
 * Borrow tokens remain trusted host data, not guest-serializable values. */
OresActorRegionV1Loan ores_actor_region_v1_borrow(OresActorRegionV1*,
    OresArenaTurn, OresActorRegionV1Ref);
void ores_actor_region_v1_release(OresActorRegionV1*, OresActorRegionV1Loan);

/* Retirement fences new accesses. Actual free waits for active turns and
 * outstanding loans; the scheduler must ALSO quiesce callbacks/wakers/FFI. */
void ores_actor_region_v1_retire(OresActorRegionV1*);
int ores_actor_region_v1_try_destroy(OresActorRegionV1*);

/* Compiler-generated native guest ABI, never an untrusted host RPC or
 * independently accessible guest API. Explicit output pointers avoid the
 * target-dependent aggregate-return C ABI for turn and reference records.
 * The LLVM lowering allocates 16-byte turn and 32-byte reference sidecars
 * with 8-byte alignment on a nonescaping native stack frame. They include
 * host-only domain, generation and turn evidence, not serializable values.
 *
 * The checked source profile rejects cancellation, suspension, exceptions,
 * methods, owned escapes and dynamic FFI before calling these functions.
 * A future fully general AOT backend must add path-sensitive cleanup and
 * scheduler-issued turn authority, not widen these wrappers by assumption. */
void ores_actor_region_v1_compiler_enter(OresActorRegionV1* region,
    uint64_t owner, uint64_t incarnation, OresArenaTurn* out);
void ores_actor_region_v1_compiler_alloc(OresActorRegionV1* region,
    const OresArenaTurn* turn, uint32_t bytes, OresActorRegionV1Ref* out);
void ores_actor_region_v1_compiler_write_i64(OresActorRegionV1* region,
    const OresArenaTurn* turn, const OresActorRegionV1Ref* ref,
    uint32_t field_offset, int64_t value);
int64_t ores_actor_region_v1_compiler_read_i64(OresActorRegionV1* region,
    const OresArenaTurn* turn, const OresActorRegionV1Ref* ref,
    uint32_t field_offset);
void ores_actor_region_v1_compiler_drop(OresActorRegionV1* region,
    const OresArenaTurn* turn, OresActorRegionV1Ref* ref);
void ores_actor_region_v1_compiler_leave(OresActorRegionV1* region,
    const OresArenaTurn* turn);

#ifdef __cplusplus
}
#endif
