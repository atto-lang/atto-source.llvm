#pragma once
#include "oreslang/runtime/ActorPackedState.h"
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Restricted compiler-generated source-actor bootstrap: a pointer to the
 * generated constructor and fixed number of actor-owned scalar fields packed into one persistent state record.
 * Returns an opaque host-owned ref; it is not a guest scalar/capability.
 * Source stop is only permitted from trusted zero-argument main, not an
 * actor carrier. Later await must use scheduler-admitted continuations.
 */
typedef void (*OresSourceActorInit)(OresActorPackedState*, uint64_t, uint64_t);
typedef int64_t (*OresSourceActorMessage)(OresActorPackedState*, uint64_t, uint64_t, int64_t);
void* oreslang_source_actor_spawn(OresSourceActorInit initializer,
                                  OresSourceActorMessage on_message,
                                  uint64_t fields);
/* Nonblocking bounded one-way send. Overload currently fails closed with a
 * host error; a typed guest overload result still needs compiler lowering.
 */
void oreslang_source_actor_send(void* actor, int64_t payload);
void oreslang_source_actor_stop(void* actor);
#ifdef __cplusplus
}
#endif
