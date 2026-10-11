#pragma once
#include "oreslang/runtime/ActorOwnedI64Heap.h"
#include "oreslang/runtime/MessagePackActorV2.h"
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Restricted compiler-generated source-actor bootstrap: a pointer to the
 * generated constructor and fixed number of actor-owned i64 fields.
 * Returns an opaque host-owned ref; it is not a guest scalar/capability.
 * Source stop is only permitted from trusted zero-argument main, not an
 * actor carrier. Later await must use scheduler-admitted continuations.
 */
typedef void (*OresSourceActorInit)(OresActorI64Heap*, uint64_t, uint64_t);
typedef int64_t (*OresSourceActorMessage)(OresActorI64Heap*, uint64_t, uint64_t, int64_t);
void* oreslang_source_actor_spawn(OresSourceActorInit initializer,
                                  OresSourceActorMessage on_message,
                                  uint64_t fields);
/* Nonblocking bounded one-way send. Overload currently fails closed with a
 * host error; a typed guest overload result still needs compiler lowering.
 */
void oreslang_source_actor_send(void* actor, int64_t payload);

/*
 * Compiler-facing MessagePack v2 source actor mode.
 *
 * Every wire DATA message uses the SAME NativeActorScheduler actor lane,
 * actor-owned i64 heap, lifecycle barriers, and CONTROL initialization as
 * ordinary scalar source actors. A compiler-trusted descriptor is stable
 * during registration; it is copied first, then independently validated.
 * Caller-owned descriptor memory is not retained.
 *
 * This entrypoint registers ONE schema per actor. General enum/pattern
 * domains with multiple variants require explicit LLVM compiler lowering
 * and per-variant dispatch and MUST NOT be represented by an unchecked
 * caller-selected schema or an extra independent scheduler.
 *
 * Wire bytes belong to the admitted callback only. Decode into actor-owned
 * storage before retaining data beyond the turn. Configuration and stop
 * require trusted single-owner lifecycle; all producers must be fenced
 * before stopping/freeing the opaque actor handle.
 */
typedef int64_t (*OresSourceActorWireMessage)(
    OresActorI64Heap*, uint64_t domain, uint64_t epoch,
    const uint8_t* wire, size_t wire_size);
void* oreslang_source_actor_spawn_wire(
    OresSourceActorInit initializer, OresSourceActorWireMessage on_wire,
    uint64_t fields, const OresMp2Contract* compiler_owned_contract);
/* Returns 1 on admission, 0 for malformed frame, full actor DATA mailbox
 * or stopped actor (not a stale-pointer-safe handle). Caller retains ownership of input bytes.
 * Registered wire actors cannot use scalar source_actor_send.
 */
int oreslang_source_actor_send_wire(
    void* actor, const uint8_t* wire, size_t size);

/* Closed typed mailbox: compiler registers a fixed ordered schema table.
 * variant_index is the compiler-local ordinal (0..contract_count-1), NOT
 * an enum's declared numeric wire tag. Schema identity is verified from
 * the MessagePack envelope and the complete typed payload is validated.
 * All variants share ONE native DATA queue, capacity budget and serial
 * actor execution lane. No runtime shape inference and no second pool.
 *
 * Registration copies all contracts before validating them. The supplied
 * contracts must remain stable for the duration of the registration call.
 * Duplicate schema IDs, invalid digests, empty/oversized domains and
 * unsupported descriptors fail closed without spawning an actor.
 *
 * send_variant returns zero for unknown/forged/noncanonical envelopes or
 * queue saturation; it never dispatches by payload-shape guessing. The
 * registered ordinal is independent of the declared enum wire tag.
 *
 * Trusted stop closes producer admission and waits for already-entered
 * source send calls before releasing contracts. Callers must still fence
 * *new* calls to opaque actor pointers before teardown starts: there is no
 * stale-pointer-safe ref-counted guest handle ABI yet.
 */
typedef int64_t (*OresSourceActorVariantMessage)(
    OresActorI64Heap*, uint64_t domain, uint64_t epoch,
    size_t variant_index, const uint8_t* wire, size_t wire_size);
void* oreslang_source_actor_spawn_variants(
    OresSourceActorInit initializer,
    OresSourceActorVariantMessage on_variant,
    uint64_t fields, const OresMp2Contract* contracts,
    size_t contract_count);
int oreslang_source_actor_send_variant(
    void* actor, const uint8_t* wire, size_t size);
/* Compiler-only fail-closed DATA send. Never silently drop source
 * `ref.send(Enum.Case)` when the bounded mailbox rejects a message.
 * Only trusted main may invoke this synchronous, no-retry admission form.
 * Future<T> backpressure is a separate, not-yet-lowered path. */
/* Only compiled receive arms may call this after nominal admission.
 * Fails closed if an invalid or differently shaped payload reaches an
 * int64 binder; arbitrary caller memory is not an acceptable input. */
int64_t oreslang_source_actor_tagged_i64(const uint8_t* wire, size_t size);
void oreslang_source_actor_send_variant_checked(
    void* actor, const uint8_t* wire, size_t size);
/* REQUIRED path for Java-compatible 'define enum' / 'define pattern':
 * a variant's SHA-256 commits to tuple SHAPE but not the 'Status' name
 * or numeric case tag inside its tuple. Pin these nominal identities at
 * compile time and verify BOTH before entering a guest receive callback.
 *
 * The generic spawn_variants API above is structural-only; it is NOT
 * sufficient by itself for typed nominal enum source messages.
 * All tag descriptors are copied before verification and registration.
 */
typedef struct {
    const char* enum_name;
    uint32_t declared_tag; /* 1..INT32_MAX; NOT the variant index */
} OresSourceNominalCase;
void* oreslang_source_actor_spawn_tagged_variants(
    OresSourceActorInit initializer,
    OresSourceActorVariantMessage on_variant, uint64_t fields,
    const OresMp2Contract* contracts,
    const OresSourceNominalCase* identities, size_t contract_count);
void oreslang_source_actor_stop(void* actor);
/* A source-compiled continuation runs on the root ActorGroup CONTROL lane.
 * Only the outer trusted-main ABI waits to return the program's i64 result;
 * no scheduler worker blocks while the readiness Future is pending. */
typedef int64_t (*OresSourceActorContinuation)(void* actor_ref);
int64_t oreslang_source_actor_await_ready(
    void* actor, OresSourceActorContinuation continuation);
#ifdef __cplusplus
}
#endif
