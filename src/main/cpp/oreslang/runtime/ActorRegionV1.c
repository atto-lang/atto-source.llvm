#include "oreslang/runtime/ActorRegionV1.h"
#include <stdint.h>
#include <stdlib.h>
#include <stddef.h>

#define ORES_REGION_MAX_SLOTS 4096u
#define ORES_REGION_MAX_SLOT_BYTES (1024u * 1024u)
#define ORES_REGION_MAX_SCRATCH_BYTES (16u * 1024u * 1024u)
#define ORES_REGION_MAX_ACTOR_BYTES (64u * 1024u * 1024u)

struct OresActorRegionV1 {
    OresActorArena* arena;
    uint32_t slots;
    uint32_t stride;
};

static void deny(void) { abort(); }
static OresActorRegionV1* require_region(OresActorRegionV1* r) {
    if (!r || !r->arena) deny();
    return r;
}
static OresActorRegionV1Ref checked_ref(OresActorRegionV1* r,
                                        OresActorRegionV1Ref ref) {
    /* The one-based offset is a four-byte guest representation, not a
     * capability or a host memory address. The ORIGINAL proof must remain
     * in trusted sidecar metadata; never infer a new proof from the offset. */
    if (ref.proof.index >= r->slots) deny();
    const uint64_t encoded = (uint64_t)ref.proof.index * r->stride + 1u;
    if (encoded > UINT32_MAX || !ref.offset.storage ||
        (uint64_t)ref.offset.storage != encoded)
        deny();
    return ref;
}
static void scrub(void* memory, size_t count) {
    volatile unsigned char* bytes = (volatile unsigned char*)memory;
    while (count--) *bytes++ = 0;
}
OresActorRegionV1* ores_actor_region_v1_open(uint64_t actor_id,
        uint64_t incarnation, uint32_t object_slots, uint32_t slot_bytes,
        uint32_t scratch_bytes) {
    const uint64_t bytes = (uint64_t)object_slots * slot_bytes;
    if (!actor_id || !incarnation || !object_slots ||
        object_slots > ORES_REGION_MAX_SLOTS || !slot_bytes ||
        slot_bytes > ORES_REGION_MAX_SLOT_BYTES || !scratch_bytes ||
        scratch_bytes > ORES_REGION_MAX_SCRATCH_BYTES ||
        bytes + scratch_bytes > ORES_REGION_MAX_ACTOR_BYTES ||
        bytes > UINT32_MAX - 1u)
        deny();
    OresActorRegionV1* region = (OresActorRegionV1*)calloc(1, sizeof(*region));
    if (!region) deny();
    region->arena = ores_actor_arena_open(actor_id, incarnation, object_slots,
                                          slot_bytes, scratch_bytes);
    region->slots = object_slots;
    region->stride = slot_bytes;
    return region;
}
int ores_actor_region_v1_prefault(OresActorRegionV1* r) {
    return ores_actor_arena_prefault(require_region(r)->arena);
}
OresArenaTurn ores_actor_region_v1_enter(OresActorRegionV1* r,
                                        uint64_t owner, uint64_t incarnation) {
    return ores_actor_arena_enter(require_region(r)->arena, owner, incarnation);
}
OresArenaTurn ores_actor_region_v1_enter_realtime(OresActorRegionV1* r,
                                        uint64_t owner, uint64_t incarnation) {
    return ores_actor_arena_enter_realtime(require_region(r)->arena, owner, incarnation);
}
void ores_actor_region_v1_leave(OresActorRegionV1* r, OresArenaTurn turn) {
    ores_actor_arena_leave(require_region(r)->arena, turn);
}
OresActorRegionV1Ref ores_actor_region_v1_alloc(OresActorRegionV1* r,
                                        OresArenaTurn turn, uint32_t bytes) {
    r = require_region(r);
    const OresArenaHandle proof = ores_actor_arena_alloc(r->arena, turn, bytes);
    const uint64_t encoded = (uint64_t)proof.index * r->stride + 1u;
    if (proof.index >= r->slots || encoded > UINT32_MAX) deny();
    const OresActorRegionV1Ref ref = {{(uint32_t)encoded}, proof};
    return ref;
}
void ores_actor_region_v1_read(OresActorRegionV1* r, OresArenaTurn turn,
                               OresActorRegionV1Ref ref, uint32_t field_offset,
                               void* out, uint32_t bytes) {
    r = require_region(r);
    ref = checked_ref(r, ref);
    ores_actor_arena_read(r->arena, turn, ref.proof, field_offset, out, bytes);
}
void ores_actor_region_v1_write(OresActorRegionV1* r, OresArenaTurn turn,
                                OresActorRegionV1Ref ref, uint32_t field_offset,
                                const void* src, uint32_t bytes) {
    r = require_region(r);
    ref = checked_ref(r, ref);
    ores_actor_arena_write(r->arena, turn, ref.proof, field_offset, src, bytes);
}
void ores_actor_region_v1_drop(OresActorRegionV1* r, OresArenaTurn turn,
                               OresActorRegionV1Ref* ref) {
    r = require_region(r);
    if (!ref) deny();
    checked_ref(r, *ref);
    ores_actor_arena_drop(r->arena, turn, ref->proof);
    /* Disarm only this local token. Other copies require their old generation
     * and must never be revalidated when the physical slot gets reused. */
    ref->offset.storage = 0;
    ref->proof.domain = 0;
    ref->proof.index = 0;
    ref->proof.generation = 0;
}
OresActorRegionV1Loan ores_actor_region_v1_borrow(OresActorRegionV1* r,
                                      OresArenaTurn turn, OresActorRegionV1Ref ref) {
    r = require_region(r);
    ref = checked_ref(r, ref);
    OresActorRegionV1Loan loan = {ref,
        ores_actor_arena_borrow(r->arena, turn, ref.proof)};
    return loan;
}
void ores_actor_region_v1_release(OresActorRegionV1* r, OresActorRegionV1Loan loan) {
    r = require_region(r);
    checked_ref(r, loan.ref);
    if (!loan.proof.nonce || loan.proof.domain != loan.ref.proof.domain ||
        loan.proof.index != loan.ref.proof.index ||
        loan.proof.generation != loan.ref.proof.generation) deny();
    /* The underlying arena authenticates loan nonce, generation and domain;
     * release intentionally remains legal during retirement. */
    ores_actor_arena_release(r->arena, loan.proof);
}
void ores_actor_region_v1_retire(OresActorRegionV1* r) {
    ores_actor_arena_retire(require_region(r)->arena);
}
int ores_actor_region_v1_try_destroy(OresActorRegionV1* r) {
    r = require_region(r);
    if (!ores_actor_arena_try_destroy(r->arena)) return 0;
    scrub(r, sizeof(*r));
    free(r);
    return 1;
}

/* Compiler-generated native guest ABI; these shims operate only on sidecars
 * allocated by validated LLVM IR. Neither a compact offset nor a naked
 * generation is an authorization token. Exact sizes/alignments are part of
 * this opt-in ABI; never silently reinterpret a different target layout.
 * The compiler must lower cancellation/error cleanups before admitting them. */
_Static_assert(sizeof(OresArenaTurn) == 16, "LLVM turn sidecar ABI drift");
_Static_assert(sizeof(OresActorRegionV1Ref) == 32, "LLVM region sidecar ABI drift");
_Static_assert(offsetof(OresActorRegionV1Ref, proof) == 8,
               "LLVM region ref sidecar alignment drift");

void ores_actor_region_v1_compiler_enter(OresActorRegionV1* region,
        uint64_t owner, uint64_t incarnation, OresArenaTurn* out) {
    if (!out) deny();
    *out = ores_actor_region_v1_enter(region, owner, incarnation);
}
void ores_actor_region_v1_compiler_alloc(OresActorRegionV1* region,
        const OresArenaTurn* turn, uint32_t bytes, OresActorRegionV1Ref* out) {
    if (!turn || !out) deny();
    *out = ores_actor_region_v1_alloc(region, *turn, bytes);
}
void ores_actor_region_v1_compiler_write_i64(OresActorRegionV1* region,
        const OresArenaTurn* turn, const OresActorRegionV1Ref* ref,
        uint32_t field_offset, int64_t value) {
    if (!turn || !ref) deny();
    ores_actor_region_v1_write(region, *turn, *ref,
                              field_offset, &value, sizeof(value));
}
int64_t ores_actor_region_v1_compiler_read_i64(OresActorRegionV1* region,
        const OresArenaTurn* turn, const OresActorRegionV1Ref* ref,
        uint32_t field_offset) {
    if (!turn || !ref) deny();
    int64_t value = 0;
    ores_actor_region_v1_read(region, *turn, *ref,
                             field_offset, &value, sizeof(value));
    return value;
}
void ores_actor_region_v1_compiler_drop(OresActorRegionV1* region,
        const OresArenaTurn* turn, OresActorRegionV1Ref* ref) {
    if (!turn || !ref) deny();
    ores_actor_region_v1_drop(region, *turn, ref);
}
void ores_actor_region_v1_compiler_leave(OresActorRegionV1* region,
        const OresArenaTurn* turn) {
    if (!turn) deny();
    ores_actor_region_v1_leave(region, *turn);
}
