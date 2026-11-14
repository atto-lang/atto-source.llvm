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
typedef uint64_t OresSourceActorId;
typedef int64_t (*OresSourceActorIdContinuation)(OresSourceActorId actor);

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

void* oreslang_source_actor_spawn(OresSourceActorInit initializer,
                                  OresSourceActorMessage on_message,
                                  uint64_t fields);

/*
 * Checked source actor identity v1: the nonzero uint64 is an opaque
 * compiler-owned actor-incarnation ID, never an address cast to an integer.
 * IDs are process-monotonic and never reused, including across group reopen
 * and scheduler slot/allocation address reuse. Exhaustion fails closed before
 * wrapping. This is a trusted compiler/native ABI, NOT a cryptographic bearer
 * capability or an FFI/untrusted actor interface.
 *
 * All checked operations resolve ID -> live SourceActor under group.mutex.
 * Checked Future continuation callbacks MUST use
 * OresSourceActorIdContinuation directly: never cast a void* callback to an
 * integer-ID callback. The legacy pointer functions remain compatibility-only
 * and are not protected from allocation-address ABA.
 */
OresSourceActorId oreslang_source_actor_spawn_id(
    OresSourceActorInit initializer,
    OresSourceActorMessage on_message,
    uint64_t fields);
int oreslang_source_actor_ready_status_id(OresSourceActorId actor);
/* Returns 1 on admitted message, 0 for invalid/revoked/closed/full.
 * The compiler can use the fail-stop variant until Result lowering exists. */
int oreslang_source_actor_try_send_id(
    OresSourceActorId actor, int64_t payload);
void oreslang_source_actor_send_id(
    OresSourceActorId actor, int64_t payload);
/* Returns 1 on first stop admission, 0 for invalid/revoked/already-stopped. */
int oreslang_source_actor_try_request_stop_id(OresSourceActorId actor);
void oreslang_source_actor_request_stop_id(OresSourceActorId actor);
/* Checked top-level physical join must never run on a native actor carrier.
 * This consumes the ID's lifetime: all subsequent operations fail closed. */
void oreslang_source_actor_join_done_id(OresSourceActorId actor);
void oreslang_source_actor_stop_id(OresSourceActorId actor);
/* Same root CONTROL Future/continuation lifecycle as the legacy await API,
 * but the callback's argument is the original minted by-value actor ID.
 * No C void* -> i64 conversion occurs anywhere in the checked ABI. */
int oreslang_source_actor_await_ready_id_checked(
    OresSourceActorId actor,
    OresSourceActorIdContinuation continuation,
    uint64_t source_token,
    int64_t* result);
int64_t oreslang_source_actor_await_ready_id(
    OresSourceActorId actor,
    OresSourceActorIdContinuation continuation,
    uint64_t source_token);
int64_t oreslang_source_actor_await_done_id(
    OresSourceActorId actor,
    OresSourceActorIdContinuation continuation,
    uint64_t source_token);

#ifdef ORESLANG_SOURCE_ACTOR_TESTING
/* Deterministic ABA fixture only: hold one *retired* SourceActor allocation
 * and hand that exact address to the next spawn. The production runtime must
 * never retain a tombstone just to make stale handles appear safe. */
void oreslang_source_actor_test_force_address_reuse(int enabled);
uint64_t oreslang_source_actor_test_address_reuses(void);
#endif

/* Nonblocking bounded one-way send. Overload currently fails closed with a
 * host error; a typed guest overload result still needs compiler lowering.
 */
void oreslang_source_actor_send(void* actor, int64_t payload);

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
