#pragma once
#include "oreslang/runtime/ActorOwnedI64Heap.h"
#include "oreslang/runtime/NativeActorScheduler.h"
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
/* Hidden compiler/runtime continuation ABI. The opaque actor handle is not a
 * guest value; it exists so resumed code can address backend-owned continuation
 * metadata without exposing scheduler refs, tickets, principals or pointers. */
typedef void (*OresSourceActorCapabilityResume)(
    void* actor, OresActorI64Heap*, uint64_t, uint64_t,
    OresNativeActorCapabilityWake wake);
void* oreslang_source_actor_spawn(OresSourceActorInit initializer,
                                  OresSourceActorMessage on_message,
                                  uint64_t fields);
void* oreslang_source_actor_spawn_resumable(
    OresSourceActorInit initializer, OresSourceActorMessage on_message,
    OresSourceActorCapabilityResume on_capability, uint64_t fields);
/* Trusted runtime admission for a previously requested compiler capability
 * ticket. Never source mailbox data and never callable with a guest ActorRef. */
int oreslang_source_actor_publish_capability_wake(
    void* actor, OresNativeActorCapabilityWake wake);
/* Nonblocking bounded one-way send. Overload currently fails closed with a
 * host error; a typed guest overload result still needs compiler lowering.
 */
void oreslang_source_actor_send(void* actor, int64_t payload);
void oreslang_source_actor_stop(void* actor);
#ifdef __cplusplus
}
#endif
