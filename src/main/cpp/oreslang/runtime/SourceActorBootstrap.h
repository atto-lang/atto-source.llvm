#pragma once
#include "oreslang/runtime/ActorOwnedI64Heap.h"
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
