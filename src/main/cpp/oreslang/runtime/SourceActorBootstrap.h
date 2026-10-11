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
/* Trusted source compiler bridge. Pipe is one-way with exclusive consumer,
 * bounded to 128 unacknowledged i64 messages. Connect before any sends.
 * A source stop waits for all forwarded messages to complete at its sink,
 * after which the consumer can be stopped separately.
 */
void oreslang_source_actor_pipe(void* producer, void* consumer);
/* Nonblocking admission: 0 means queue/pipe credits unavailable, ownership
 * remains with the caller. Guest void send still traps fail-closed on rejection.
 */
int oreslang_source_actor_try_send(void* actor, int64_t payload);
void oreslang_source_actor_send(void* actor, int64_t payload);
void oreslang_source_actor_stop(void* actor);
#ifdef __cplusplus
}
#endif
