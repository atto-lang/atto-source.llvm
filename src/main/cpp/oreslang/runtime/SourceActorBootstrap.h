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
 * ordinary scalar source actors. The registration contract is verified,
 * copied and pinned at spawn (not borrowed from caller-owned memory).
 * Source compiler must typecheck guest payloads before emitting these calls.
 *
 * Borrowed wire bytes are valid only during the admitted callback. If source
 * code retains decoded data, it must allocate in actor-owned memory.
 */
typedef int64_t (*OresSourceActorWireMessage)(
    OresActorI64Heap*, uint64_t domain, uint64_t epoch,
    const uint8_t* wire, size_t wire_size);
void* oreslang_source_actor_spawn_wire(
    OresSourceActorInit initializer, OresSourceActorWireMessage on_wire,
    uint64_t fields, const OresMp2Contract* compiler_owned_contract);
/* Returns 1 on admission, 0 for malformed frame, full actor DATA mailbox
 * or stopped/stale actor. Caller retains ownership of input bytes.
 * Registered wire actors cannot use scalar source_actor_send.
 */
int oreslang_source_actor_send_wire(
    void* actor, const uint8_t* wire, size_t size);
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
