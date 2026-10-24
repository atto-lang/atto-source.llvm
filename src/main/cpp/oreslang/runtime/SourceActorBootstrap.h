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
typedef int64_t (*OresSourceActorOutputContinuation)(
    OresActorI64Heap*, uint64_t, uint64_t, int64_t, int64_t);

#define ORES_SOURCE_ACTOR_CAPABILITY_SLOTS 8u

void* oreslang_source_actor_spawn(OresSourceActorInit initializer,
                                  OresSourceActorMessage on_message,
                                  uint64_t fields);
/* Runtime-only foundation for future compiler-lowered actor capabilities.
 * Each input must be a live source ActorRef from the same active group.
 * The runtime copies only the generation-fenced native actor identity into
 * the new actor; it never retains the caller's SourceActor* pointer.
 * Source syntax for binding these slots remains separately gated.
 */
void* oreslang_source_actor_spawn_with_capabilities(
    OresSourceActorInit initializer,
    OresSourceActorMessage on_message,
    uint64_t fields,
    void* const* actors,
    uint64_t actor_count);
/* Nonblocking bounded one-way send. Strict send fails closed on any rejection.
 * try_send returns 0 only for mailbox saturation; stale/terminated/invalid
 * references remain fatal runtime contract violations.
 */
void oreslang_source_actor_send(void* actor, int64_t payload);
int oreslang_source_actor_try_send(void* actor, int64_t payload);
/* Handler-only capability send. heap must be the current actor's private heap,
 * and slot must name a runtime-copied generation-fenced capability. These
 * functions never expose a raw target pointer to guest scalar state.
 */
void oreslang_source_actor_send_capability(
    OresActorI64Heap* heap, uint64_t slot, int64_t payload);
int oreslang_source_actor_try_send_capability(
    OresActorI64Heap* heap, uint64_t slot, int64_t payload);
/* Actor output is a separate bounded parent-observable stream, matching
 * source self.send(...); it is not a self-mailbox enqueue. take_output is
 * trusted-host blocking only and must never run on an actor carrier. */
void oreslang_source_actor_emit_output(
    OresActorI64Heap* heap, int64_t payload);
int64_t oreslang_source_actor_take_output(void* actor);
/* Handler-only nonblocking lifecycle request matching source self.end(). */
void oreslang_source_actor_request_end(OresActorI64Heap* heap);
/* First actor-handler suspension slice: wait for the next output from an owned
 * child capability without parking the carrier. DATA for the parent remains
 * bounded/admitted but gated until the CONTROL continuation resumes. */
void oreslang_source_actor_await_child_output(
    OresActorI64Heap* heap,
    uint64_t capability,
    int64_t original_mail,
    OresSourceActorOutputContinuation continuation,
    uint64_t source_token);
/* Constructor-only structured child binding. The current actor must own heap,
 * slot must be the next dense capability slot, and the child is retained only
 * by trusted runtime metadata plus its generation-fenced capability identity.
 */
void oreslang_source_actor_spawn_child_capability(
    OresActorI64Heap* heap,
    uint64_t slot,
    OresSourceActorInit initializer,
    OresSourceActorMessage on_message,
    uint64_t fields);
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
