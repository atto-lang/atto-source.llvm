#pragma once
#include "oreslang/runtime/ActorOwnedI64Heap.h"
#include "oreslang/runtime/NativeLogicalTrace.h"
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Restricted compiler-generated source-actor bootstrap: a pointer to the
 * generated constructor and fixed number of actor-owned i64 fields.
 * Returns an opaque host-owned ref; it is not a guest scalar/capability.
 * Source stop is only permitted from trusted zero-argument main.
 */
typedef void (*OresSourceActorInit)(OresActorI64Heap*, uint64_t, uint64_t);
typedef int64_t (*OresSourceActorMessage)(
    OresActorI64Heap*, uint64_t, uint64_t, int64_t);
typedef int64_t (*OresSourceActorContinuation)(void* actor);

enum {
    ORES_SOURCE_ACTOR_STARTUP_PENDING = 0,
    ORES_SOURCE_ACTOR_STARTUP_INITIALIZING = 1,
    ORES_SOURCE_ACTOR_STARTUP_READY = 2,
    ORES_SOURCE_ACTOR_STARTUP_TERMINATED_BEFORE_READY = 3
};

/* Read-only host lifecycle observation. TERMINATED_BEFORE_READY corresponds
 * to a failed get_ready observer while get_done still completes normally. */
int oreslang_source_actor_ready_status(void* actor);

#ifdef ORESLANG_SOURCE_ACTOR_TESTING
/* Deterministic startup-race proof hooks. They pause only the reserved
 * initialization CONTROL callback before it decides whether construction may
 * begin; they are not linked into ordinary source programs. */
void oreslang_source_actor_test_hold_initialization(int hold);
int oreslang_source_actor_test_initialization_entered(void);
void oreslang_source_actor_test_hold_readiness_publication(int hold);
int oreslang_source_actor_test_readiness_publication_entered(void);
#endif

#define ORES_SOURCE_ACTOR_CAPABILITY_SLOTS 8u

void* oreslang_source_actor_spawn(OresSourceActorInit initializer,
                                  OresSourceActorMessage on_message,
                                  uint64_t fields);
/* Runtime-only host bridge for copied peer capabilities. Each input must be a
 * live source ActorRef in this active group. The runtime copies generation-
 * fenced identities; caller SourceActor pointers are not retained as guest
 * state. Compiler-generated child fields use spawn_child_capability below. */
void* oreslang_source_actor_spawn_with_capabilities(
    OresSourceActorInit initializer,
    OresSourceActorMessage on_message,
    uint64_t fields,
    void* const* actors,
    uint64_t actor_count);

/* Nonblocking bounded one-way send. Strict send fails closed on any rejection.
 * try_send returns 0 only for mailbox saturation; stale/terminated/invalid
 * references remain runtime contract violations.
 */
void oreslang_source_actor_send(void* actor, int64_t payload);
int oreslang_source_actor_try_send(void* actor, int64_t payload);

/* Compiler-owned ActorRef<T> child capability ABI. Child creation is legal
 * only during the current actor constructor; capability send is legal only
 * during that actor's handler and must present its exact private heap. */
void oreslang_source_actor_spawn_child_capability(
    OresActorI64Heap* heap,
    uint64_t slot,
    OresSourceActorInit initializer,
    OresSourceActorMessage on_message,
    uint64_t fields);
void oreslang_source_actor_send_capability(
    OresActorI64Heap* heap, uint64_t slot, int64_t payload);
int oreslang_source_actor_try_send_capability(
    OresActorI64Heap* heap, uint64_t slot, int64_t payload);

/*
 * Compiler/runtime stop phases. request_stop is nonblocking and only asks the
 * scheduler to terminate the actor. join_done is trusted host glue: it waits
 * for physical VM completion, retires/destroys the actor heap, releases the
 * opaque SourceActor allocation, and closes an idle source-actor VM.
 *
 * Source lowering without get_done continues to call oreslang_source_actor_stop(),
 * which keeps its existing synchronous request+join behavior. get_done lowering
 * uses request_stop plus oreslang_source_actor_await_done(); the split APIs remain
 * trusted compiler/runtime capabilities, not guest-callable functions.
 */
void oreslang_source_actor_request_stop(void* actor);
void oreslang_source_actor_join_done(void* actor);
void oreslang_source_actor_stop(void* actor);

/*
 * Restricted compiler-generated await slice:
 *   await actor.get_ready();
 *
 * The trusted host arranges one root CONTROL suspension turn, tears down that
 * worker stack, then a NativeActorFuture admits exactly one root CONTROL resume
 * turn. The persisted OresNativeSuspensionLink carries logical ancestry between
 * those turns; no native stack pointer is retained.
 *
 * The continuation runs on the root actor lane and must not synchronously call
 * source_actor_stop. The outer trusted main performs stop after this function
 * returns. source_token is a compiler-provided source offset/site identifier.
 */
int64_t oreslang_source_actor_await_ready(
    void* actor,
    OresSourceActorContinuation continuation,
    uint64_t source_token);

/* Host/runtime projection of the same readiness observer. Returns READY or
 * TERMINATED_BEFORE_READY; the continuation runs only on READY. The compiler
 * wrapper above remains fail-closed until typed ActorTerminated propagation is
 * lowered for source await. */
int oreslang_source_actor_await_ready_checked(
    void* actor,
    OresSourceActorContinuation continuation,
    uint64_t source_token,
    int64_t* result);

/*
 * Restricted compiler-generated terminal await slice:
 *   stop actor;
 *   await actor.get_done();
 *
 * The stop request has already been admitted before this call. The trusted
 * outer host, never an actor carrier, waits for the target's physical terminal
 * ACK. A root CONTROL suspension/resume pair preserves logical ancestry; the
 * target actor heap and opaque SourceActor are reclaimed only after the resumed
 * continuation has completed.
 */
int64_t oreslang_source_actor_await_done(
    void* actor,
    OresSourceActorContinuation continuation,
    uint64_t source_token);

/*
 * Active only while a compiler-generated source continuation is executing.
 * These hooks let throw/raise/panic rendering attach the live logical ancestry
 * without exposing a native stack pointer or runtime store object.
 */
int oreslang_source_actor_current_execution_handle(
    OresNativeExecutionHandle* out);
int oreslang_source_actor_snapshot_current_trace(
    OresNativeExecutionTraceFrame* execution_frames,
    size_t execution_capacity,
    OresNativeContinuationTraceFrame* continuation_frames,
    size_t continuation_capacity,
    OresNativeLogicalTraceResult* out);

#ifdef __cplusplus
}
#endif
