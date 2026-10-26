#pragma once
#include "oreslang/runtime/ActorOwnedI64Heap.h"
#include "oreslang/runtime/NativeActorScheduler.h"
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
/* Detached send continuation with runtime-owned Copy-scalar state. The state
 * pointer is valid only for the authenticated resume callback and must not
 * escape or be retained by guest code. */
typedef int64_t (*OresSourceActorStateContinuation)(
    void* actor, const int64_t* state, uint64_t state_count);

enum {
    ORES_SOURCE_DETACHED_SCALAR_WORDS = 16,
    ORES_SOURCE_DETACHED_MAX_PCS = 4,
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
/* Deterministic quota-path hook for the isolated send_async runtime test.
 * Refuses values above the production cohort bound. */
int oreslang_source_actor_test_set_async_waiters(void* actor, size_t waiters);
size_t oreslang_source_actor_test_pending_async_sends(void* actor);
/* Test-only group-mutex snapshot: a host observer or cancellation owns
 * the opaque Future across its out-of-lock scheduler operation. */
int oreslang_source_actor_test_future_consuming(void* future);
/* True after exactly one outer host has claimed detached frame finalization. */
int oreslang_source_actor_test_detached_finalizer_claimed(void);
/* True only after detached finalization has cleared callback/Future identity,
 * state count and every bounded scalar word before the embedded frame is reused. */
int oreslang_source_actor_test_detached_state_cleared(void);
/* True only while join_done has reached the actor reclamation fence and is
 * waiting specifically for live async waiters/Future wrappers to retire. */
int oreslang_source_actor_test_join_waiting_on_futures(void* actor);
#endif

void* oreslang_source_actor_spawn(OresSourceActorInit initializer,
                                  OresSourceActorMessage on_message,
                                  uint64_t fields);
/* Nonblocking bounded one-way send. Overload currently fails closed with a
 * host error; a typed guest overload result still needs compiler lowering.
 */
/* Typed host/runtime projection of immediate fail-fast send. The compiler's
 * existing send() wrapper still aborts on any non-ACCEPTED result. */
int oreslang_source_actor_try_send_checked(void* actor, int64_t payload);
void oreslang_source_actor_send(void* actor, int64_t payload);

/*
 * First source send_async vertical slice.
 *
 * The checked form is trusted host/runtime glue for a direct
 *   await actor.send_async(i64)
 * from zero-argument main. Submission is a scheduler-owned nonblocking Future;
 * this compatibility wrapper then waits only to observe that Future's terminal
 * admission state. No actor carrier may call the observing wait; such a call
 * returns WOULD_DEADLOCK instead of parking a carrier.
 *
 * Values 0..6 deliberately preserve OresNativeSendStatus for future typed
 * compiler projection. WOULD_DEADLOCK is source-runtime specific.
 */
enum {
    /* Match the Java bounded pending-producer cohort. Scheduler Future
     * descriptors and source opaque Future wrappers are both bounded here. */
    ORES_SOURCE_SEND_ASYNC_MAX_WAITERS = 256,
    ORES_SOURCE_SEND_ASYNC_ACCEPTED = ORES_NATIVE_SEND_ACCEPTED,
    ORES_SOURCE_SEND_ASYNC_INVALID_REF = ORES_NATIVE_SEND_INVALID_REF,
    ORES_SOURCE_SEND_ASYNC_TERMINATED = ORES_NATIVE_SEND_TERMINATED,
    ORES_SOURCE_SEND_ASYNC_MAILBOX_FULL = ORES_NATIVE_SEND_MAILBOX_FULL,
    ORES_SOURCE_SEND_ASYNC_RESOURCE_EXHAUSTED =
        ORES_NATIVE_SEND_RESOURCE_EXHAUSTED,
    ORES_SOURCE_SEND_ASYNC_TRANSPORT_UNAVAILABLE =
        ORES_NATIVE_SEND_TRANSPORT_UNAVAILABLE,
    ORES_SOURCE_SEND_ASYNC_CANCELLED = ORES_NATIVE_SEND_CANCELLED,
    ORES_SOURCE_SEND_ASYNC_WOULD_DEADLOCK = 7
};
int oreslang_source_actor_await_send_async_checked(
    void* actor, int64_t payload);
void oreslang_source_actor_await_send_async(
    void* actor, int64_t payload);

/*
 * Linear opaque Future<void> form. Submission starts immediately and returns a
 * runtime-owned handle backed by the actor's fixed 256-slot wrapper pool plus
 * the scheduler's generation-fenced async-send descriptor. The compiler must
 * consume the handle exactly once with await or cancellation.
 */
int oreslang_source_actor_send_async_start_checked(
    void* actor, int64_t payload, void** future_out);
void* oreslang_source_actor_send_async_start(
    void* actor, int64_t payload);
/* Host-only terminal observer retained for low-level runtime tests. */
int oreslang_source_actor_await_send_future_checked(void* future);
/* Trusted-host cleanup for a started Future whose compiler handoff failed.
 * Pending admission is cancelled if possible; otherwise terminal settlement
 * is observed. In both cases the descriptor/wrapper is consumed exactly once
 * and the exact final send status is returned. Never call from a carrier. */
int oreslang_source_actor_discard_send_future_checked(void* future);
/*
 * Compiler-facing logical await. The guest continuation is suspended on the
 * root actor and the exact scheduler async-send descriptor is bound to a
 * generation-fenced producer wake. Mailbox settlement publishes that wake
 * directly; no trusted host thread polls or waits on async-send settlement.
 *
 * This compatibility C ABI is still synchronous to its outer caller while the
 * resumed guest continuation finishes. Full host-stack detachment therefore
 * remains a compiler state-machine step rather than a runtime-settlement claim.
 */
int oreslang_source_actor_await_send_future_continuation_checked(
    void* future,
    OresSourceActorContinuation continuation,
    uint64_t source_token,
    int64_t* result_out);
int64_t oreslang_source_actor_await_send_future(
    void* future,
    OresSourceActorContinuation continuation,
    uint64_t source_token);

/*
 * Detached compiler-main vertical slice. detach establishes runtime ownership
 * of the suspended Future and returns without waiting for suspension capture,
 * mailbox admission, or continuation execution. The root CONTROL turn captures
 * the logical continuation and binds the exact scheduler descriptor itself.
 *
 * wait_detached_send_result is single-consumer outer-host/process glue:
 * exactly one external host may claim finalization. It waits only for the
 * detached function to finish, releases continuation/Future metadata, performs
 * physical actor join after nonblocking stop admission, and returns the exact
 * send outcome. Concurrent/duplicate finalizers fail closed with INVALID_REF.
 * It must never run on an actor carrier.
 */
int oreslang_source_actor_detach_send_future_checked(
    void* future,
    OresSourceActorContinuation continuation,
    uint64_t source_token);
/* State-bearing compiler entry. Copies at most 16 scalar words into the
 * runtime-owned detached frame before returning to the guest caller.
 *
 * This is a trusted compiler/runtime ABI, not a guest type-erasure boundary:
 * lowering must admit only Copy int/bool locals, canonicalize bool to exactly
 * 0/1, and never encode pointers, ActorRef capabilities, borrows, strings,
 * sums, classes, arrays or other owned values as state words. The runtime
 * deliberately does not infer source types from the copied i64 payload.
 * state may be NULL only when state_count is zero. */
int oreslang_source_actor_detach_send_future_state_checked(
    void* future,
    OresSourceActorStateContinuation continuation,
    uint64_t source_token,
    const int64_t* state,
    uint64_t state_count);

/*
 * Carrier-only next-PC transition for the detached direct send_async state
 * machine. It checked-submits the next payload, snapshots Copy scalar state,
 * and publishes the next logical suspension from the currently resumed
 * execution with the current continuation as parent.
 */
int oreslang_source_actor_rearm_detached_send_state_checked(
    void* actor,
    int64_t payload,
    OresSourceActorStateContinuation continuation,
    uint64_t source_token,
    const int64_t* state,
    uint64_t state_count);

int oreslang_source_actor_wait_detached_send_result(int64_t* result_out);

int oreslang_source_actor_cancel_send_future(void* future);

/* Exact trusted identity projection for runtime integration/tests. This copies
 * the scheduler-minted generation-fenced ActorRef stored in the opaque source
 * handle; it never reconstructs identity from an actor id or numeric slot. */
int oreslang_source_actor_exact_scheduler_ref(
    void* actor, OresNativeActorRef* out);

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
