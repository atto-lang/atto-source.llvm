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
void* oreslang_source_actor_spawn(OresSourceActorInit initializer, uint64_t fields);
void oreslang_source_actor_stop(void* actor);
#ifdef __cplusplus
}
#endif
