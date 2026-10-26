#include "oreslang/runtime/SourceActorBootstrap.h"
#include "oreslang/runtime/NativeOresVM.h"
#include "oreslang/runtime/NativeAwaitTraceBridge.h"
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define SOURCE_ACTOR_MAGIC UINT64_C(0x4f52455353504157)
#define SOURCE_GROUP_SLOTS 64u
#define SOURCE_GROUP_ROOT_SLOT (SOURCE_GROUP_SLOTS - 1u)
#define SOURCE_GROUP_CARRIERS 4u
#define SOURCE_GROUP_MAILBOX 128u
#define SOURCE_TRACE_EXECUTIONS 128u
#define SOURCE_TRACE_CONTINUATIONS 128u
#define SOURCE_ROOT_CONTROL_SUSPEND UINT64_C(1)
#define SOURCE_ROOT_CONTROL_RESUME UINT64_C(2)
#define SOURCE_ROOT_CONTROL_DONE_SUSPEND UINT64_C(3)
#define SOURCE_ROOT_CONTROL_DONE_RESUME UINT64_C(4)
#define SOURCE_ROOT_CONTROL_SEND_SUSPEND UINT64_C(5)
#define SOURCE_ROOT_CONTROL_SELECT_SUSPEND UINT64_C(6)

typedef struct SourceActor SourceActor;
typedef struct {
    SourceActor* actor;
    OresNativeAsyncSendTicket ticket;
    uint64_t generation;
    int live;
    /* Under group.mutex: exactly one host observer OR canceller owns this
     * one-shot wrapper while scheduler calls run outside that mutex. */
    int consuming;
} SourceSendFuture;

struct SourceActor {
    uint64_t magic;
    uint64_t id;
    OresActorI64Heap* heap;
    OresNativeActorRef ref;
    OresSourceActorInit constructor;
    OresSourceActorMessage on_message;
    atomic_int initialized;
    atomic_int startup_state;
    atomic_int constructor_started;
    atomic_int stop_requested;
    atomic_int join_started;
    atomic_uint_fast64_t processed;
    atomic_int_fast64_t last_result;
    /* Protected by group.mutex. Keeps the opaque allocation alive while an
     * outer-host send_async admission wait temporarily releases that mutex. */
    size_t async_waiters;
    SourceSendFuture send_futures[ORES_SOURCE_SEND_ASYNC_MAX_WAITERS];
    size_t send_future_live;
};

typedef struct {
    pthread_cond_t completed;
    OresNativeActorFuture* ready_future;
    OresSourceActorContinuation continuation;
    SourceActor* actor;
    OresNativeSuspensionLink link;
    uint64_t source_token;
    int suspend_recorded;
    int done;
    int outcome;
    int64_t result;
} ReadyAwaitFrame;

typedef struct {
    pthread_cond_t completed;
    OresNativeActorFuture* done_future;
    OresSourceActorContinuation continuation;
    SourceActor* actor;
    OresNativeSuspensionLink link;
    uint64_t source_token;
    int suspend_recorded;
    int done;
    int64_t result;
} DoneAwaitFrame;

typedef struct {
    pthread_cond_t completed;
    OresSourceActorContinuation continuation;
    OresSourceActorStateContinuation state_continuation;
    uint64_t detached_state_count;
    int64_t detached_state[ORES_SOURCE_DETACHED_SCALAR_WORDS];
    SourceSendFuture* send_future;
    SourceActor* actor;
    OresNativeSuspensionLink link;
    uint64_t source_token;
    uint32_t pc;
    int suspend_recorded;
    int detached;
    int finalizer_claimed;
    int wake_bound;
    int done;
    int outcome;
    int64_t result;

    /* One next-PC staging lane. It is populated only by the currently resumed
     * root carrier and promoted by that wake's epilogue after the current
     * continuation has been finished. This avoids exposing a half-promoted
     * frame to a later scheduler wake. */
    int rearm_requested;
    int rearm_terminal;
    int rearm_outcome;
    SourceSendFuture* next_send_future;
    OresSourceActorStateContinuation next_state_continuation;
    uint64_t next_source_token;
    uint64_t next_state_count;
    int64_t next_state[ORES_SOURCE_DETACHED_SCALAR_WORDS];
    OresNativeSuspensionLink next_link;

    /* Descriptor release is external-host-only. Completed PC descriptors and
     * an unbound next-PC descriptor (if suspension minting fails) stay pinned
     * here until the outer finalizer runs after all wake callbacks are off-stack. */
    uint32_t completed_send_count;
    SourceSendFuture* completed_send_futures[
        ORES_SOURCE_DETACHED_MAX_PCS - 1];
    SourceSendFuture* abandoned_send_future;
} SendAwaitFrame;

typedef struct {
    pthread_cond_t completed;
    OresSourceSelectSend2Continuation continuation;
    SourceActor* actors[2];
    int64_t payloads[2];

    /* Runtime-owned for the full detached lifetime. These objects must not
     * live on a guest/native stack because scheduler wake credits retain the
     * arbiter pointer until physical callback retirement. */
    OresNativeSelectArbiter arbiter;
    uint64_t selection_generation;
    OresNativeSelectToken tokens[2];
    OresNativeSelectWakeTicket common_wake;
    OresNativeAsyncSendTicket sends[2];
    int send_live[2];
    int immediate_status[2];

    OresNativeSuspensionLink link;
    uint64_t source_token;
    int suspend_recorded;
    int wake_reserved;
    int setup_complete;
    int finalizer_claimed;
    int done;
    int outcome;
    uint64_t winner_arm;
    int64_t result;
} SelectAwaitFrame;

typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t async_waiters_changed;
    OresNativeVM* vm;
    OresNativeActorRef root_ref;
    OresNativeExecutionContextStore* executions;
    OresNativeContinuationStore* continuations;
    ReadyAwaitFrame* pending_ready;
    DoneAwaitFrame* pending_done;
    SendAwaitFrame* pending_send;
    SelectAwaitFrame* pending_select;
    /* Bounded compiler-main continuation frames. Only one suspended root
     * operation is admitted at a time. */
    SendAwaitFrame detached_send;
    SelectAwaitFrame detached_select;
    SourceActor* actors[SOURCE_GROUP_SLOTS];
    size_t next_slot;
    size_t live;
    size_t peak;
} SourceActorGroup;

/* Process-local trusted-main group; the compiler only supports linear refs
 * in a zero-argument top-level main. There is exactly one worker pool per
 * active group, not one pool per actor.
 */
static SourceActorGroup group = {
    .mutex = PTHREAD_MUTEX_INITIALIZER,
    .async_waiters_changed = PTHREAD_COND_INITIALIZER
};
static _Atomic uint64_t next_actor_id = 4100;
static _Atomic uint64_t next_select_generation = 1;
static _Thread_local OresNativeExecutionHandle current_source_execution;

/* The test-only admission hook below uses these helpers before their
 * definitions. C11 requires declarations before first use, including when
 * ORESLANG_SOURCE_ACTOR_TESTING is enabled under -Werror. */
static void group_lock(void);
static void group_unlock(void);
static int source_send_future_valid_locked(SourceSendFuture* future);
static void retire_consuming_send_future_terminal(
    SourceSendFuture* future, SourceActor* actor);
static void abandon_consuming_send_future_unbound(
    SourceSendFuture* future, SourceActor* actor);

#ifdef ORESLANG_SOURCE_ACTOR_TESTING
static atomic_int test_hold_initialization;
static atomic_int test_initialization_entered;
static atomic_int test_hold_readiness_publication;
static atomic_int test_readiness_publication_entered;
static atomic_uintptr_t test_join_waiting_actor;

void oreslang_source_actor_test_hold_initialization(int hold) {
    if (hold) {
        atomic_store_explicit(
            &test_initialization_entered, 0, memory_order_release);
        atomic_store_explicit(
            &test_hold_initialization, 1, memory_order_release);
    } else {
        atomic_store_explicit(
            &test_hold_initialization, 0, memory_order_release);
    }
}

int oreslang_source_actor_test_initialization_entered(void) {
    return atomic_load_explicit(
        &test_initialization_entered, memory_order_acquire);
}

void oreslang_source_actor_test_hold_readiness_publication(int hold) {
    if (hold) {
        atomic_store_explicit(
            &test_readiness_publication_entered, 0, memory_order_release);
        atomic_store_explicit(
            &test_hold_readiness_publication, 1, memory_order_release);
    } else {
        atomic_store_explicit(
            &test_hold_readiness_publication, 0, memory_order_release);
    }
}

int oreslang_source_actor_test_readiness_publication_entered(void) {
    return atomic_load_explicit(
        &test_readiness_publication_entered, memory_order_acquire);
}

/* Test-only lock-protected observation of a Future's one-shot owner.
 * Compare wrapper addresses before dereferencing, so stale test pointers
 * cannot alias a different live Future. Never enabled in production builds. */
int oreslang_source_actor_test_future_consuming(void* opaque_future) {
    if (!opaque_future) return 0;
    group_lock();
    int result = 0;
    for (size_t slot = 0; slot < group.next_slot; ++slot) {
        SourceActor* actor = group.actors[slot];
        if (!actor || actor->magic != SOURCE_ACTOR_MAGIC) continue;
        for (size_t index = 0; index < ORES_SOURCE_SEND_ASYNC_MAX_WAITERS;
                ++index) {
            SourceSendFuture* future = &actor->send_futures[index];
            if ((void*)future == opaque_future) {
                result = future->live && future->consuming;
                goto finished;
            }
        }
    }
finished:
    group_unlock();
    return result;
}

int oreslang_source_actor_test_detached_finalizer_claimed(void) {
    int claimed = 0;
    group_lock();
    SendAwaitFrame* frame = &group.detached_send;
    claimed = group.pending_send == frame
        && frame->detached && frame->finalizer_claimed;
    group_unlock();
    return claimed;
}

int oreslang_source_actor_test_detached_state_cleared(void) {
    int cleared = 1;
    group_lock();
    SendAwaitFrame* frame = &group.detached_send;
    cleared = group.pending_send != frame
        && frame->continuation == NULL
        && frame->state_continuation == NULL
        && frame->detached_state_count == 0
        && frame->send_future == NULL
        && frame->actor == NULL
        && frame->link.execution_id.runtime_domain == 0
        && frame->link.execution_id.execution == 0
        && !ores_native_store_handle_valid(frame->link.continuation)
        && !ores_native_store_handle_valid(frame->link.parent)
        && frame->source_token == 0
        && frame->pc == 0
        && !frame->suspend_recorded
        && !frame->detached
        && !frame->finalizer_claimed
        && !frame->wake_bound
        && !frame->done
        && frame->outcome == 0
        && frame->result == 0
        && !frame->rearm_requested
        && !frame->rearm_terminal
        && frame->rearm_outcome == 0
        && frame->next_send_future == NULL
        && frame->next_state_continuation == NULL
        && frame->next_source_token == 0
        && frame->next_state_count == 0
        && frame->next_link.execution_id.runtime_domain == 0
        && frame->next_link.execution_id.execution == 0
        && !ores_native_store_handle_valid(frame->next_link.continuation)
        && !ores_native_store_handle_valid(frame->next_link.parent)
        && frame->completed_send_count == 0
        && frame->abandoned_send_future == NULL;
    for (size_t i = 0;
            cleared && i < ORES_SOURCE_DETACHED_SCALAR_WORDS; ++i)
        cleared = frame->detached_state[i] == 0
            && frame->next_state[i] == 0;
    for (size_t i = 0;
            cleared && i < ORES_SOURCE_DETACHED_MAX_PCS - 1; ++i)
        cleared = frame->completed_send_futures[i] == NULL;
    group_unlock();
    return cleared;
}

int oreslang_source_actor_test_join_waiting_on_futures(void* opaque) {
    return opaque != NULL
        && atomic_load_explicit(
            &test_join_waiting_actor, memory_order_acquire)
            == (uintptr_t)opaque;
}

int oreslang_source_actor_test_set_async_waiters(
        void* opaque, size_t waiters) {
    SourceActor* instance = (SourceActor*)opaque;
    if (!instance || waiters > ORES_SOURCE_SEND_ASYNC_MAX_WAITERS) return 0;
    group_lock();
    const int ok = instance->magic == SOURCE_ACTOR_MAGIC
        && group.vm
        && instance->ref.actor_slot < group.next_slot
        && group.actors[instance->ref.actor_slot] == instance;
    if (ok) instance->async_waiters = waiters;
    group_unlock();
    return ok;
}

size_t oreslang_source_actor_test_pending_async_sends(void* opaque) {
    SourceActor* instance = (SourceActor*)opaque;
    if (!instance) return 0;
    group_lock();
    size_t count = 0;
    if (instance->magic == SOURCE_ACTOR_MAGIC && group.vm
            && instance->ref.actor_slot < group.next_slot
            && group.actors[instance->ref.actor_slot] == instance)
        count = ores_native_vm_test_pending_async_sends(
            group.vm, ORES_NATIVE_VM_MAIN, instance->ref);
    group_unlock();
    return count;
}
#endif

static void ensure(int condition) { if (!condition) abort(); }
static void group_lock(void) { ensure(pthread_mutex_lock(&group.mutex) == 0); }
static void group_unlock(void) { ensure(pthread_mutex_unlock(&group.mutex) == 0); }

static int same_allocation(
        OresNativeAllocationDomainRef a,
        OresNativeAllocationDomainRef b) {
    return a.kind == b.kind
        && a.domain == b.domain
        && a.owner == b.owner
        && a.generation == b.generation;
}

static SourceActor* lookup(size_t index) {
    ensure(index < SOURCE_GROUP_ROOT_SLOT);
    SourceActor* actor = group.actors[index];
    ensure(actor && actor->magic == SOURCE_ACTOR_MAGIC);
    return actor;
}

int oreslang_source_actor_ready_status(void* opaque) {
    SourceActor* instance = (SourceActor*)opaque;
    if (!instance || instance->magic != SOURCE_ACTOR_MAGIC) return -1;
    return atomic_load_explicit(
        &instance->startup_state, memory_order_acquire);
}

typedef struct {
    OresNativeActorFuture* future;
    int status;
} SourceReadyFutureCommit;

static int commit_source_ready_future(void* raw) {
    SourceReadyFutureCommit* commit = (SourceReadyFutureCommit*)raw;
    return commit
        && commit->future
        && ores_native_actor_future_complete(commit->future, commit->status);
}

static void settle_ready_if_admitted_locked(ReadyAwaitFrame* frame) {
    ensure(frame && frame == group.pending_ready);
    if (!frame->suspend_recorded) return;

    const int startup_state = atomic_load_explicit(
        &frame->actor->startup_state, memory_order_acquire);
    int future_status = ORES_NATIVE_FUTURE_PENDING;
    if (startup_state == ORES_SOURCE_ACTOR_STARTUP_READY)
        future_status = ORES_NATIVE_FUTURE_READY;
    else if (startup_state
            == ORES_SOURCE_ACTOR_STARTUP_TERMINATED_BEFORE_READY)
        future_status = ORES_NATIVE_FUTURE_FAILED;
    else
        return;

    ensure(frame->ready_future != NULL);
    if (ores_native_actor_future_status(frame->ready_future)
            == ORES_NATIVE_FUTURE_PENDING) {
        /*
         * Serialize the fallible Future outcome commit and continuation READY
         * publication against cancellation under the continuation-store lock.
         * Both successful readiness and TERMINATED_BEFORE_READY must wake the
         * root CONTROL lane exactly once; only the Future outcome differs.
         */
        SourceReadyFutureCommit commit = {
            .future = frame->ready_future,
            .status = future_status
        };
        ensure(ores_native_continuation_store_commit_ready(
            group.continuations,
            frame->link.continuation,
            commit_source_ready_future,
            &commit));
    }
}

static void root_suspend_turn(void) {
    group_lock();
    ReadyAwaitFrame* frame = group.pending_ready;
    ensure(frame && !frame->done && !frame->suspend_recorded);

    OresNativeExecutionContext physical = {0};
    ensure(ores_native_vm_current_execution_context(
        group.vm, ORES_NATIVE_VM_MAIN, &physical));
    ensure(physical.allocation.kind == ORES_ALLOCATION_DOMAIN_ACTOR);
    ensure(physical.allocation.owner == SOURCE_GROUP_ROOT_SLOT);

    OresNativeExecutionHandle origin = {0, 0};
    ensure(ores_native_execution_context_store_insert(
        group.executions, &physical, &origin));
    OresNativeContinuationHandle no_parent = {0, 0};
    ensure(ores_native_await_trace_suspend(
        group.executions,
        group.continuations,
        origin,
        no_parent,
        ORES_CONTINUATION_BOUNDARY_AWAIT,
        1,
        frame->source_token,
        &frame->link));

    /*
     * The published continuation owns the durable pin. Drop this callback's
     * insertion reference before its native worker stack is torn down.
     */
    ensure(ores_native_execution_context_store_release(
        group.executions, origin));
    frame->suspend_recorded = 1;
    settle_ready_if_admitted_locked(frame);
    group_unlock();
}

static void root_resume_turn(void) {
    ReadyAwaitFrame* frame;
    OresSourceActorContinuation continuation;
    SourceActor* actor;
    OresNativeSuspensionLink link;
    int future_status;

    group_lock();
    frame = group.pending_ready;
    ensure(frame && frame->suspend_recorded && !frame->done);
    ensure(frame->ready_future != NULL);
    future_status = ores_native_actor_future_status(frame->ready_future);
    ensure(future_status == ORES_NATIVE_FUTURE_READY
        || future_status == ORES_NATIVE_FUTURE_FAILED);
    continuation = frame->continuation;
    actor = frame->actor;
    link = frame->link;
    group_unlock();

    OresNativeExecutionContext physical = {0};
    ensure(ores_native_vm_current_execution_context(
        group.vm, ORES_NATIVE_VM_MAIN, &physical));
    ensure(physical.allocation.kind == ORES_ALLOCATION_DOMAIN_ACTOR);
    ensure(physical.allocation.owner == SOURCE_GROUP_ROOT_SLOT);

    OresNativeExecutionHandle resumed = {0, 0};
    OresNativeExecutionContext logical = {0};
    ensure(ores_native_await_trace_ready_resume_begin(
        group.executions,
        group.continuations,
        physical.id,
        link,
        &resumed,
        &logical));

    /*
     * Even failed readiness resumes once on the root CONTROL lane so the
     * persisted logical suspension can be retired without leaking trace state.
     * Guest continuation code runs only for successful readiness.
     */
    ensure(same_allocation(logical.allocation, physical.allocation));
    int64_t result = 0;
    int outcome = ORES_SOURCE_ACTOR_STARTUP_TERMINATED_BEFORE_READY;
    if (future_status == ORES_NATIVE_FUTURE_READY) {
        ensure(!ores_native_store_handle_valid(current_source_execution));
        current_source_execution = resumed;
        result = continuation(actor);
        current_source_execution = (OresNativeExecutionHandle){0, 0};
        outcome = ORES_SOURCE_ACTOR_STARTUP_READY;
    }

    ensure(ores_native_await_trace_resume_finish(group.continuations, link));
    ensure(ores_native_execution_context_store_release(
        group.executions, resumed));

    group_lock();
    ensure(frame == group.pending_ready && !frame->done);
    frame->outcome = outcome;
    frame->result = result;
    frame->done = 1;
    ensure(pthread_cond_signal(&frame->completed) == 0);
    group_unlock();
}

static void root_done_suspend_turn(void) {
    group_lock();
    DoneAwaitFrame* frame = group.pending_done;
    ensure(frame && !frame->done && !frame->suspend_recorded);

    OresNativeExecutionContext physical = {0};
    ensure(ores_native_vm_current_execution_context(
        group.vm, ORES_NATIVE_VM_MAIN, &physical));
    ensure(physical.allocation.kind == ORES_ALLOCATION_DOMAIN_ACTOR);
    ensure(physical.allocation.owner == SOURCE_GROUP_ROOT_SLOT);

    OresNativeExecutionHandle origin = {0, 0};
    ensure(ores_native_execution_context_store_insert(
        group.executions, &physical, &origin));
    OresNativeContinuationHandle no_parent = {0, 0};
    ensure(ores_native_await_trace_suspend(
        group.executions,
        group.continuations,
        origin,
        no_parent,
        ORES_CONTINUATION_BOUNDARY_AWAIT,
        1,
        frame->source_token,
        &frame->link));
    ensure(ores_native_execution_context_store_release(
        group.executions, origin));

    frame->suspend_recorded = 1;
    ensure(pthread_cond_signal(&frame->completed) == 0);
    group_unlock();
}

static void root_done_resume_turn(void) {
    DoneAwaitFrame* frame;
    OresSourceActorContinuation continuation;
    SourceActor* actor;
    OresNativeSuspensionLink link;

    group_lock();
    frame = group.pending_done;
    ensure(frame && frame->suspend_recorded && !frame->done);
    ensure(frame->done_future != NULL);
    ensure(ores_native_actor_future_status(frame->done_future)
        == ORES_NATIVE_FUTURE_READY);
    continuation = frame->continuation;
    actor = frame->actor;
    link = frame->link;
    group_unlock();

    OresNativeExecutionContext physical = {0};
    ensure(ores_native_vm_current_execution_context(
        group.vm, ORES_NATIVE_VM_MAIN, &physical));
    ensure(physical.allocation.kind == ORES_ALLOCATION_DOMAIN_ACTOR);
    ensure(physical.allocation.owner == SOURCE_GROUP_ROOT_SLOT);

    OresNativeExecutionHandle resumed = {0, 0};
    OresNativeExecutionContext logical = {0};
    ensure(ores_native_await_trace_resume_begin(
        group.executions,
        group.continuations,
        physical.id,
        link,
        &resumed,
        &logical));
    ensure(same_allocation(logical.allocation, physical.allocation));
    ensure(!ores_native_store_handle_valid(current_source_execution));
    current_source_execution = resumed;
    const int64_t result = continuation(actor);
    current_source_execution = (OresNativeExecutionHandle){0, 0};

    ensure(ores_native_await_trace_resume_finish(group.continuations, link));
    ensure(ores_native_execution_context_store_release(
        group.executions, resumed));

    group_lock();
    ensure(frame == group.pending_done && !frame->done);
    frame->result = result;
    frame->done = 1;
    ensure(pthread_cond_signal(&frame->completed) == 0);
    group_unlock();
}

static void root_send_suspend_turn(void) {
    group_lock();
    SendAwaitFrame* frame = group.pending_send;
    ensure(frame && !frame->done && !frame->suspend_recorded);

    OresNativeExecutionContext physical = {0};
    ensure(ores_native_vm_current_execution_context(
        group.vm, ORES_NATIVE_VM_MAIN, &physical));
    ensure(physical.allocation.kind == ORES_ALLOCATION_DOMAIN_ACTOR);
    ensure(physical.allocation.owner == SOURCE_GROUP_ROOT_SLOT);

    OresNativeExecutionHandle origin = {0, 0};
    ensure(ores_native_execution_context_store_insert(
        group.executions, &physical, &origin));
    OresNativeContinuationHandle no_parent = {0, 0};
    ensure(ores_native_await_trace_suspend(
        group.executions,
        group.continuations,
        origin,
        no_parent,
        ORES_CONTINUATION_BOUNDARY_AWAIT,
        1,
        frame->source_token,
        &frame->link));
    ensure(ores_native_execution_context_store_release(
        group.executions, origin));

    frame->suspend_recorded = 1;

    if (frame->detached) {
        /* A detached guest has already returned, so the root turn itself owns
         * descriptor-to-continuation binding. This removes the last setup wait
         * from the suspended guest/host call stack. The scheduler callback is
         * never inline and cannot acquire group.mutex until this turn unlocks. */
        ensure(frame == &group.detached_send);
        ensure(frame->send_future != NULL
            && frame->send_future->live
            && frame->send_future->consuming
            && frame->send_future->actor == frame->actor);
        OresNativeWakeTicket wake = {0};
        if (ores_native_vm_bind_async_send_wake(
                group.vm,
                ORES_NATIVE_VM_MAIN,
                frame->send_future->ticket,
                group.root_ref,
                frame->link.continuation.slot,
                frame->link.continuation.generation,
                1,
                &wake)) {
            frame->wake_bound = 1;
        } else {
            /* Preserve bounded fail-closed behavior without resurrecting the
             * detached guest stack. Outer-host finalization cancels/releases
             * the unbound descriptor and pending logical continuation. */
            frame->outcome = ORES_SOURCE_SEND_ASYNC_RESOURCE_EXHAUSTED;
            frame->done = 1;
        }
    }

    ensure(pthread_cond_signal(&frame->completed) == 0);
    group_unlock();
}

static void root_select_suspend_turn(void) {
    group_lock();
    SelectAwaitFrame* frame = group.pending_select;
    ensure(frame == &group.detached_select
        && !frame->done && !frame->suspend_recorded);

    OresNativeExecutionContext physical = {0};
    ensure(ores_native_vm_current_execution_context(
        group.vm, ORES_NATIVE_VM_MAIN, &physical));
    ensure(physical.allocation.kind == ORES_ALLOCATION_DOMAIN_ACTOR);
    ensure(physical.allocation.owner == SOURCE_GROUP_ROOT_SLOT);

    OresNativeExecutionHandle origin = {0, 0};
    ensure(ores_native_execution_context_store_insert(
        group.executions, &physical, &origin));
    OresNativeContinuationHandle no_parent = {0, 0};
    ensure(ores_native_await_trace_suspend(
        group.executions,
        group.continuations,
        origin,
        no_parent,
        ORES_CONTINUATION_BOUNDARY_AWAIT,
        1,
        frame->source_token,
        &frame->link));
    ensure(ores_native_execution_context_store_release(
        group.executions, origin));
    frame->suspend_recorded = 1;

    uint64_t generation = atomic_fetch_add_explicit(
        &next_select_generation, UINT64_C(1), memory_order_relaxed);
    if (!generation || generation == UINT64_MAX) abort();
    frame->selection_generation = generation;

    int setup_ok = ores_native_select_arbiter_init(
        &frame->arbiter, generation, 2);
    if (setup_ok)
        setup_ok = ores_native_select_register(
            &frame->arbiter, 0, &frame->tokens[0]);
    if (setup_ok)
        setup_ok = ores_native_select_register(
            &frame->arbiter, 1, &frame->tokens[1]);

    if (setup_ok) {
        setup_ok = ores_native_vm_reserve_select_wake(
            group.vm,
            ORES_NATIVE_VM_MAIN,
            group.root_ref,
            &frame->arbiter,
            generation,
            frame->link.continuation.slot,
            frame->link.continuation.generation,
            1,
            &frame->common_wake);
        frame->wake_reserved = setup_ok;
    }

    for (size_t i = 0; setup_ok && i < 2; ++i) {
        const OresNativeSendStatus status =
            ores_native_vm_submit_selectable_async_with_wake(
                group.vm,
                ORES_NATIVE_VM_MAIN,
                frame->actors[i]->ref,
                (uint64_t)frame->payloads[i],
                frame->tokens[i],
                frame->common_wake,
                &frame->sends[i]);
        frame->immediate_status[i] = (int)status;
        frame->send_live[i] = frame->sends[i].generation != 0;

        /* Descriptor/waiter exhaustion is selection setup failure. Terminal
         * target state is a selectable event and CANCELLED means another arm
         * already committed, so neither is setup failure here. */
        if (status == ORES_NATIVE_SEND_RESOURCE_EXHAUSTED
                || status == ORES_NATIVE_SEND_TRANSPORT_UNAVAILABLE)
            setup_ok = 0;
    }

    if (!setup_ok) {
        for (size_t i = 0; i < 2; ++i) {
            if (frame->send_live[i])
                (void)ores_native_vm_cancel_async_send(
                    group.vm, ORES_NATIVE_VM_MAIN, frame->sends[i]);
        }
        (void)ores_native_select_seal(&frame->arbiter);
        if (frame->wake_reserved) {
            ensure(ores_native_vm_cancel_select_wake(
                group.vm, ORES_NATIVE_VM_MAIN, frame->common_wake));
            frame->wake_reserved = 0;
        }
        ensure(ores_native_continuation_store_cancel(
            group.continuations, frame->link.continuation));
        frame->outcome = ORES_SOURCE_SEND_ASYNC_RESOURCE_EXHAUSTED;
        frame->winner_arm = UINT64_MAX;
        frame->done = 1;
    } else {
        frame->setup_complete = 1;
    }

    ensure(pthread_cond_signal(&frame->completed) == 0);
    group_unlock();
}

static void source_select_wake(
        OresNativeWakeTicket ticket,
        OresNativeWakeOutcome wake_outcome) {
    SelectAwaitFrame* frame;
    OresSourceSelectSend2Continuation continuation;
    SourceActor* actor0;
    SourceActor* actor1;
    OresNativeSuspensionLink link;
    uint64_t winner_arm;
    int outcome;

    group_lock();
    frame = group.pending_select;
    ensure(frame == &group.detached_select
        && frame->suspend_recorded && frame->setup_complete && !frame->done);
    ensure(wake_outcome == ORES_NATIVE_WAKE_READY);
    ensure(ticket.producer.scheduler_domain == group.root_ref.scheduler_domain
        && ticket.producer.actor_slot == group.root_ref.actor_slot
        && ticket.producer.generation == group.root_ref.generation
        && ticket.target.scheduler_domain == group.root_ref.scheduler_domain
        && ticket.target.actor_slot == group.root_ref.actor_slot
        && ticket.target.generation == group.root_ref.generation);
    ensure(ticket.continuation_slot == frame->link.continuation.slot
        && ticket.continuation_generation == frame->link.continuation.generation
        && ticket.code_generation == 1
        && ticket.descriptor_slot == UINT64_MAX
        && ticket.descriptor_generation == frame->selection_generation
        && ticket.completion_code >= 1
        && ticket.completion_code <= 2);

    winner_arm = ticket.completion_code - UINT64_C(1);
    uint64_t recorded_winner = UINT64_MAX;
    ensure(ores_native_select_winner(
        &frame->arbiter, &recorded_winner));
    ensure(recorded_winner == winner_arm);

    outcome = frame->immediate_status[winner_arm];
    if (frame->send_live[winner_arm]) {
        OresNativeAsyncSendState state = ORES_NATIVE_ASYNC_SEND_INVALID;
        OresNativeSendStatus exact = ORES_NATIVE_SEND_INVALID_REF;
        ensure(ores_native_vm_async_send_status(
            group.vm, ORES_NATIVE_VM_MAIN,
            frame->sends[winner_arm], &state, &exact));
        ensure(state == ORES_NATIVE_ASYNC_SEND_READY
            || state == ORES_NATIVE_ASYNC_SEND_FAILED);
        outcome = (int)exact;
    }
    ensure(outcome == ORES_SOURCE_SEND_ASYNC_ACCEPTED
        || outcome == ORES_SOURCE_SEND_ASYNC_TERMINATED
        || outcome == ORES_SOURCE_SEND_ASYNC_INVALID_REF
        || outcome == ORES_SOURCE_SEND_ASYNC_RESOURCE_EXHAUSTED);

    ensure(ores_native_continuation_store_notify_ready(
        group.continuations, frame->link.continuation));
    continuation = frame->continuation;
    actor0 = frame->actors[0];
    actor1 = frame->actors[1];
    link = frame->link;
    frame->winner_arm = winner_arm;
    frame->outcome = outcome;
    group_unlock();

    OresNativeExecutionContext physical = {0};
    ensure(ores_native_vm_current_execution_context(
        group.vm, ORES_NATIVE_VM_MAIN, &physical));
    ensure(physical.allocation.kind == ORES_ALLOCATION_DOMAIN_ACTOR);
    ensure(physical.allocation.owner == SOURCE_GROUP_ROOT_SLOT);

    OresNativeExecutionHandle resumed = {0, 0};
    OresNativeExecutionContext logical = {0};
    ensure(ores_native_await_trace_ready_resume_begin(
        group.executions,
        group.continuations,
        physical.id,
        link,
        &resumed,
        &logical));
    ensure(same_allocation(logical.allocation, physical.allocation));

    int64_t result = 0;
    if (outcome == ORES_SOURCE_SEND_ASYNC_ACCEPTED) {
        ensure(continuation != NULL);
        ensure(!ores_native_store_handle_valid(current_source_execution));
        current_source_execution = resumed;
        result = continuation(actor0, actor1, winner_arm);
        current_source_execution = (OresNativeExecutionHandle){0, 0};
    }

    ensure(ores_native_await_trace_resume_finish(group.continuations, link));
    ensure(ores_native_execution_context_store_release(
        group.executions, resumed));

    group_lock();
    ensure(frame == group.pending_select && !frame->done);
    frame->result = result;
    frame->done = 1;
    ensure(pthread_cond_signal(&frame->completed) == 0);
    group_unlock();
}

static void source_send_wake(
        void* raw, size_t slot, OresNativeWakeTicket ticket,
        OresNativeWakeOutcome wake_outcome) {
    ensure(raw == &group && slot == SOURCE_GROUP_ROOT_SLOT);
    ensure(ores_native_vm_is_current_wake(
        group.vm, ORES_NATIVE_VM_MAIN, ticket));

    if (ticket.descriptor_slot == UINT64_MAX) {
        source_select_wake(ticket, wake_outcome);
        return;
    }

    SendAwaitFrame* frame;
    OresSourceActorContinuation continuation;
    OresSourceActorStateContinuation state_continuation;
    const int64_t* detached_state;
    uint64_t detached_state_count;
    SourceActor* actor;
    OresNativeSuspensionLink link;
    int send_outcome;

    group_lock();
    frame = group.pending_send;
    ensure(frame && frame->suspend_recorded && !frame->done);
    ensure(frame->send_future != NULL
        && frame->send_future->live
        && frame->send_future->consuming
        && frame->send_future->actor == frame->actor);
    ensure(ticket.producer.scheduler_domain == group.root_ref.scheduler_domain
        && ticket.producer.actor_slot == group.root_ref.actor_slot
        && ticket.producer.generation == group.root_ref.generation);
    ensure(ticket.target.scheduler_domain
            == frame->send_future->ticket.target.scheduler_domain
        && ticket.target.actor_slot
            == frame->send_future->ticket.target.actor_slot
        && ticket.target.generation
            == frame->send_future->ticket.target.generation);
    ensure(ticket.continuation_slot == frame->link.continuation.slot
        && ticket.continuation_generation == frame->link.continuation.generation
        && ticket.code_generation == 1
        && ticket.descriptor_slot == frame->send_future->ticket.slot
        && ticket.descriptor_generation == frame->send_future->ticket.generation);

    send_outcome = (int)ticket.completion_code;
    if (wake_outcome == ORES_NATIVE_WAKE_READY)
        ensure(send_outcome == ORES_SOURCE_SEND_ASYNC_ACCEPTED);
    else if (wake_outcome == ORES_NATIVE_WAKE_FAILED)
        ensure(send_outcome == ORES_SOURCE_SEND_ASYNC_TERMINATED
            || send_outcome == ORES_SOURCE_SEND_ASYNC_RESOURCE_EXHAUSTED);
    else if (wake_outcome == ORES_NATIVE_WAKE_CANCELLED)
        ensure(send_outcome == ORES_SOURCE_SEND_ASYNC_CANCELLED);
    else
        abort();

    /* Scheduler settlement owns the READY transition. There is no host-side
     * async-send status observer between mailbox admission and logical resume. */
    ensure(ores_native_continuation_store_notify_ready(
        group.continuations, frame->link.continuation));
    continuation = frame->continuation;
    state_continuation = frame->state_continuation;
    detached_state = frame->detached_state;
    detached_state_count = frame->detached_state_count;
    ensure((continuation != NULL) != (state_continuation != NULL));
    ensure(detached_state_count <= ORES_SOURCE_DETACHED_SCALAR_WORDS);
    actor = frame->actor;
    link = frame->link;
    frame->outcome = send_outcome;
    group_unlock();

    OresNativeExecutionContext physical = {0};
    ensure(ores_native_vm_current_execution_context(
        group.vm, ORES_NATIVE_VM_MAIN, &physical));
    ensure(physical.allocation.kind == ORES_ALLOCATION_DOMAIN_ACTOR);
    ensure(physical.allocation.owner == SOURCE_GROUP_ROOT_SLOT);

    OresNativeExecutionHandle resumed = {0, 0};
    OresNativeExecutionContext logical = {0};
    ensure(ores_native_await_trace_ready_resume_begin(
        group.executions,
        group.continuations,
        physical.id,
        link,
        &resumed,
        &logical));
    ensure(same_allocation(logical.allocation, physical.allocation));

    int64_t result = 0;
    if (send_outcome == ORES_SOURCE_SEND_ASYNC_ACCEPTED) {
        ensure(!ores_native_store_handle_valid(current_source_execution));
        current_source_execution = resumed;
        result = state_continuation
            ? state_continuation(actor, detached_state, detached_state_count)
            : continuation(actor);
        current_source_execution = (OresNativeExecutionHandle){0, 0};
    }

    ensure(ores_native_await_trace_resume_finish(group.continuations, link));
    ensure(ores_native_execution_context_store_release(
        group.executions, resumed));

    group_lock();
    ensure(frame == group.pending_send && !frame->done);

    if (frame->rearm_requested) {
        if (frame->rearm_terminal) {
            /*
             * The next PC failed before a descriptor/wake pair could become
             * authoritative. Preserve that exact failure for the outer host;
             * the current descriptor remains the frame's final cleanup owner.
             */
            frame->result = 0;
            frame->outcome = frame->rearm_outcome;
            frame->done = 1;
            ensure(pthread_cond_signal(&frame->completed) == 0);
            group_unlock();
            return;
        }

        SourceSendFuture* previous = frame->send_future;
        SourceSendFuture* next = frame->next_send_future;
        SourceActor* rearm_actor = frame->actor;
        ensure(previous && next && previous != next);
        ensure(source_send_future_valid_locked(previous)
            && previous->consuming && previous->actor == rearm_actor);
        ensure(source_send_future_valid_locked(next)
            && next->consuming && next->actor == rearm_actor);
        ensure(frame->next_state_continuation != NULL
            && frame->next_state_count <= ORES_SOURCE_DETACHED_SCALAR_WORDS
            && ores_native_store_handle_valid(
                frame->next_link.continuation));

        /* The next continuation owns a retained parent reference. Drop the
         * completed PC's direct frame-owned reference before overwriting
         * frame->link; ancestry remains live through the child and is retired
         * transitively when the final child is released. */
        ensure(ores_native_continuation_store_release(
            group.continuations, link.continuation));

        ensure(frame->completed_send_count
            < ORES_SOURCE_DETACHED_MAX_PCS - 1);
        frame->completed_send_futures[
            frame->completed_send_count++] = previous;
        ensure(frame->next_send_future == next
            && source_send_future_valid_locked(next)
            && next->consuming && next->actor == rearm_actor);

        frame->send_future = next;
        frame->continuation = NULL;
        frame->state_continuation = frame->next_state_continuation;
        frame->detached_state_count = frame->next_state_count;
        for (uint64_t i = 0; i < ORES_SOURCE_DETACHED_SCALAR_WORDS; ++i)
            frame->detached_state[i] = 0;
        for (uint64_t i = 0; i < frame->next_state_count; ++i)
            frame->detached_state[i] = frame->next_state[i];
        frame->source_token = frame->next_source_token;
        frame->link = frame->next_link;
        ++frame->pc;
        ensure(frame->pc < ORES_SOURCE_DETACHED_MAX_PCS);
        frame->suspend_recorded = 1;
        frame->wake_bound = 0;
        frame->outcome = ORES_SOURCE_SEND_ASYNC_INVALID_REF;

        frame->rearm_requested = 0;
        frame->rearm_terminal = 0;
        frame->rearm_outcome = ORES_SOURCE_SEND_ASYNC_INVALID_REF;
        frame->next_send_future = NULL;
        frame->next_state_continuation = NULL;
        frame->next_source_token = 0;
        frame->next_state_count = 0;
        for (uint64_t i = 0; i < ORES_SOURCE_DETACHED_SCALAR_WORDS; ++i)
            frame->next_state[i] = 0;
        frame->next_link = (OresNativeSuspensionLink){0};

        OresNativeWakeTicket next_wake = {0};
        if (ores_native_vm_bind_async_send_wake(
                group.vm,
                ORES_NATIVE_VM_MAIN,
                frame->send_future->ticket,
                group.root_ref,
                frame->link.continuation.slot,
                frame->link.continuation.generation,
                1,
                &next_wake)) {
            frame->wake_bound = 1;
        } else {
            /*
             * The next logical suspension exists but no scheduler wake was
             * reserved. Outer-host finalization cancels/releases both the
             * descriptor and this still-PENDING continuation exactly once.
             */
            frame->outcome = ORES_SOURCE_SEND_ASYNC_RESOURCE_EXHAUSTED;
            frame->done = 1;
            ensure(pthread_cond_signal(&frame->completed) == 0);
        }
        group_unlock();
        return;
    }

    frame->result = result;
    frame->done = 1;
    ensure(pthread_cond_signal(&frame->completed) == 0);
    group_unlock();
}

static void initialize(void* raw, size_t slot, uint64_t payload) {
    ensure(raw == &group);
    if (slot == SOURCE_GROUP_ROOT_SLOT) {
        if (payload == SOURCE_ROOT_CONTROL_SUSPEND) root_suspend_turn();
        else if (payload == SOURCE_ROOT_CONTROL_RESUME) root_resume_turn();
        else if (payload == SOURCE_ROOT_CONTROL_DONE_SUSPEND) root_done_suspend_turn();
        else if (payload == SOURCE_ROOT_CONTROL_DONE_RESUME) root_done_resume_turn();
        else if (payload == SOURCE_ROOT_CONTROL_SEND_SUSPEND) root_send_suspend_turn();
        else if (payload == SOURCE_ROOT_CONTROL_SELECT_SUSPEND) root_select_suspend_turn();
        else abort();
        return;
    }

    ensure(payload == 0);
    SourceActor* instance = lookup(slot);

#ifdef ORESLANG_SOURCE_ACTOR_TESTING
    if (atomic_load_explicit(
            &test_hold_initialization, memory_order_acquire)) {
        atomic_store_explicit(
            &test_initialization_entered, 1, memory_order_release);
        while (atomic_load_explicit(
                &test_hold_initialization, memory_order_acquire))
            sched_yield();
    }
#endif

    /*
     * Match Java ActorRef.stop(): startup admission and stop admission
     * linearize under the same group mutex. The reserved initialization
     * CONTROL turn survives mailbox sealing, but stop-before-start skips guest
     * construction completely.
     */
    group_lock();
    if (atomic_load_explicit(
            &instance->stop_requested, memory_order_acquire)) {
        atomic_store_explicit(
            &instance->startup_state,
            ORES_SOURCE_ACTOR_STARTUP_TERMINATED_BEFORE_READY,
            memory_order_release);
        ReadyAwaitFrame* frame = group.pending_ready;
        if (frame && frame->actor == instance)
            settle_ready_if_admitted_locked(frame);
        group_unlock();
        return;
    }

    int expected = ORES_SOURCE_ACTOR_STARTUP_PENDING;
    ensure(atomic_compare_exchange_strong_explicit(
        &instance->startup_state,
        &expected,
        ORES_SOURCE_ACTOR_STARTUP_INITIALIZING,
        memory_order_acq_rel,
        memory_order_acquire));
    atomic_store_explicit(
        &instance->constructor_started, 1, memory_order_release);
    group_unlock();

    OresActorTurn turn =
        ores_actor_i64_heap_enter(instance->heap, instance->id, 1);
    instance->constructor(instance->heap, turn.domain, turn.epoch);
    ores_actor_i64_heap_leave(instance->heap, turn);

#ifdef ORESLANG_SOURCE_ACTOR_TESTING
    if (atomic_load_explicit(
            &test_hold_readiness_publication, memory_order_acquire)) {
        atomic_store_explicit(
            &test_readiness_publication_entered, 1, memory_order_release);
        while (atomic_load_explicit(
                &test_hold_readiness_publication, memory_order_acquire))
            sched_yield();
    }
#endif

    /*
     * Stop may race an already-running constructor. Re-enter the same group
     * mutex before publishing readiness: if stop acquired it first, readiness
     * fails; if this turn publishes READY first, a later stop cannot retroactively
     * change the already-settled readiness observer.
     */
    group_lock();
    if (atomic_load_explicit(
            &instance->stop_requested, memory_order_acquire)) {
        atomic_store_explicit(
            &instance->startup_state,
            ORES_SOURCE_ACTOR_STARTUP_TERMINATED_BEFORE_READY,
            memory_order_release);
    } else {
        atomic_store_explicit(
            &instance->initialized, 1, memory_order_release);
        atomic_store_explicit(
            &instance->startup_state,
            ORES_SOURCE_ACTOR_STARTUP_READY,
            memory_order_release);
    }

    ReadyAwaitFrame* frame = group.pending_ready;
    if (frame && frame->actor == instance)
        settle_ready_if_admitted_locked(frame);
    group_unlock();
}

static void deliver(void* raw, size_t slot, uint64_t payload) {
    ensure(raw == &group && slot != SOURCE_GROUP_ROOT_SLOT);
    SourceActor* instance = lookup(slot);
    ensure(instance->on_message != NULL);
    ensure(atomic_load_explicit(
        &instance->initialized, memory_order_acquire) == 1);
    OresActorTurn turn =
        ores_actor_i64_heap_enter(instance->heap, instance->id, 1);
    const int64_t result = instance->on_message(
        instance->heap, turn.domain, turn.epoch, (int64_t)payload);
    ores_actor_i64_heap_leave(instance->heap, turn);
    atomic_store_explicit(
        &instance->last_result, result, memory_order_relaxed);
    atomic_fetch_add_explicit(
        &instance->processed, 1, memory_order_release);

}

static void open_group_locked(void) {
    ensure(!group.vm && !group.executions && !group.continuations);
    ensure(group.live == 0 && group.next_slot == 0 && group.peak == 0);
    ensure(group.pending_ready == NULL && group.pending_done == NULL
        && group.pending_send == NULL
        && group.pending_select == NULL);

    /*
     * Live trace stores are process-local runtime metadata. They hold logical
     * values/handles only and outlive individual worker stacks.
     */
    group.executions =
        ores_native_execution_context_store_open(SOURCE_TRACE_EXECUTIONS);
    ensure(group.executions != NULL);
    group.continuations = ores_native_continuation_store_open(
        SOURCE_TRACE_CONTINUATIONS, group.executions);
    ensure(group.continuations != NULL);

    OresNativeVMConfig cfg = {0};
    for (int i = 0; i < ORES_NATIVE_VM_DOMAIN_COUNT; ++i) {
        cfg.domains[i].actor_slots =
            i == ORES_NATIVE_VM_MAIN ? SOURCE_GROUP_SLOTS : 1;
        cfg.domains[i].mailbox_capacity =
            i == ORES_NATIVE_VM_MAIN ? SOURCE_GROUP_MAILBOX : 1;
        cfg.domains[i].carrier_workers =
            i == ORES_NATIVE_VM_MAIN ? SOURCE_GROUP_CARRIERS : 1;
        cfg.domains[i].on_message = deliver;
        cfg.domains[i].on_control = initialize;
        cfg.domains[i].context = &group;
    }
    group.vm = ores_native_vm_open(&cfg);
    ensure(group.vm != NULL);
    ensure(ores_native_vm_configure_wake_callback(
        group.vm, ORES_NATIVE_VM_MAIN, source_send_wake));

    /*
     * The compiler-generated top-level continuation owns one reserved logical
     * actor lane. It is metadata-only until an await schedules CONTROL.
     */
    group.root_ref = ores_native_vm_spawn(
        group.vm, ORES_NATIVE_VM_MAIN, SOURCE_GROUP_ROOT_SLOT);
    ensure(group.root_ref.scheduler_domain
        && group.root_ref.actor_slot == SOURCE_GROUP_ROOT_SLOT);

#ifdef ORESLANG_SOURCE_ACTOR_TESTING
    for (int i = 0; i < ORES_NATIVE_VM_DOMAIN_COUNT; ++i)
        ensure(ores_native_vm_started_carriers(
            group.vm, (enum OresNativeVMDomain)i) == 0);
    fprintf(stderr,
        "SOURCE_ACTOR_VM_ROUTING_VERIFIED domains=4 main=1 cold_carriers=0 root_slot=%u\n",
        SOURCE_GROUP_ROOT_SLOT);
#endif
}

void* oreslang_source_actor_spawn(
        OresSourceActorInit constructor,
        OresSourceActorMessage on_message,
        uint64_t fields) {
    ensure(constructor != NULL && fields >= 1 && fields <= 4096);
    group_lock();
    if (!group.vm) open_group_locked();

    ensure(group.next_slot < SOURCE_GROUP_ROOT_SLOT);
    const size_t slot = group.next_slot++;
    SourceActor* instance = (SourceActor*)calloc(1, sizeof(*instance));
    ensure(instance != NULL);
    instance->magic = SOURCE_ACTOR_MAGIC;
    instance->id =
        atomic_fetch_add_explicit(&next_actor_id, 1, memory_order_relaxed);
    ensure(instance->id != 0 && instance->id != UINT64_MAX);
    instance->constructor = constructor;
    instance->on_message = on_message;

    /*
     * SourceActor is dynamically allocated. Initialize every C11 atomic
     * explicitly instead of relying on calloc byte patterns to stand in for
     * atomic object initialization.
     */
    atomic_init(&instance->initialized, 0);
    atomic_init(
        &instance->startup_state, ORES_SOURCE_ACTOR_STARTUP_PENDING);
    atomic_init(&instance->constructor_started, 0);
    atomic_init(&instance->stop_requested, 0);
    atomic_init(&instance->join_started, 0);
    atomic_init(&instance->processed, 0);
    atomic_init(&instance->last_result, 0);

    instance->heap = ores_actor_i64_heap_open(instance->id, 1, fields);
    group.actors[slot] = instance;
    instance->ref =
        ores_native_vm_spawn(group.vm, ORES_NATIVE_VM_MAIN, slot);
    ensure(instance->ref.scheduler_domain
        && instance->ref.actor_slot == slot);
    group.live++;
    if (group.live > group.peak) group.peak = group.live;

    /* CONTROL initializes the compiler-emitted actor constructor on a worker. */
    ensure(ores_native_vm_publish_control(
        group.vm, ORES_NATIVE_VM_MAIN, instance->ref, 0));
    group_unlock();
    return instance;
}

int oreslang_source_actor_try_send_checked(void* opaque, int64_t payload) {
    SourceActor* instance = (SourceActor*)opaque;
    if (!instance) return ORES_NATIVE_SEND_INVALID_REF;
    group_lock();
    int result = ORES_NATIVE_SEND_INVALID_REF;
    if (instance->magic == SOURCE_ACTOR_MAGIC && instance->on_message && group.vm) {
        const size_t slot = (size_t)instance->ref.actor_slot;
        if (slot < group.next_slot && group.actors[slot] == instance)
            result = (int)ores_native_vm_try_send(
                group.vm, ORES_NATIVE_VM_MAIN, instance->ref, (uint64_t)payload);
    }
    group_unlock();
    return result;
}

void oreslang_source_actor_send(void* opaque, int64_t payload) {
    ensure(oreslang_source_actor_try_send_checked(opaque, payload)
        == ORES_NATIVE_SEND_ACCEPTED);
}

int oreslang_source_actor_exact_scheduler_ref(
        void* opaque, OresNativeActorRef* out) {
    SourceActor* instance = (SourceActor*)opaque;
    if (!instance || !out) return 0;
    group_lock();
    int ok = 0;
    if (instance->magic == SOURCE_ACTOR_MAGIC && group.vm) {
        const size_t slot = (size_t)instance->ref.actor_slot;
        if (slot < group.next_slot && group.actors[slot] == instance
                && instance->ref.scheduler_domain) {
            *out = instance->ref;
            ok = 1;
        }
    }
    group_unlock();
    return ok;
}

int oreslang_source_actor_await_send_async_checked(
        void* opaque, int64_t payload) {
    SourceActor* instance = (SourceActor*)opaque;
    if (!instance) return ORES_SOURCE_SEND_ASYNC_INVALID_REF;

    group_lock();
    if (instance->magic != SOURCE_ACTOR_MAGIC || !group.vm
            || !instance->on_message) {
        group_unlock();
        return ORES_SOURCE_SEND_ASYNC_INVALID_REF;
    }
    if (atomic_load_explicit(
            &instance->stop_requested, memory_order_acquire)) {
        group_unlock();
        return ORES_SOURCE_SEND_ASYNC_TERMINATED;
    }
    const size_t slot = (size_t)instance->ref.actor_slot;
    if (slot >= group.next_slot || group.actors[slot] != instance) {
        group_unlock();
        return ORES_SOURCE_SEND_ASYNC_INVALID_REF;
    }

    /*
     * The Future itself is scheduler-owned and nonblocking, but this checked
     * direct-await compatibility wrapper waits for its terminal state. Never
     * park any actor carrier on that host observation.
     */
    if (ores_native_actor_scheduler_is_carrier_thread()) {
        group_unlock();
        return ORES_SOURCE_SEND_ASYNC_WOULD_DEADLOCK;
    }

    /* Pin the opaque SourceActor/VM while the externally awaited Future is
     * live. The scheduler independently enforces the same 256 descriptor
     * cohort for nonblocking submissions. */
    if (instance->async_waiters >= ORES_SOURCE_SEND_ASYNC_MAX_WAITERS) {
        group_unlock();
        return ORES_SOURCE_SEND_ASYNC_RESOURCE_EXHAUSTED;
    }
    ++instance->async_waiters;
    OresNativeVM* vm = group.vm;
    const OresNativeActorRef target = instance->ref;
    group_unlock();

    OresNativeAsyncSendTicket ticket = {0};
    OresNativeSendStatus outcome = ores_native_vm_submit_async(
        vm, ORES_NATIVE_VM_MAIN, target, (uint64_t)payload, &ticket);
    if (outcome == ORES_NATIVE_SEND_ACCEPTED) {
        ensure(ticket.generation != 0);
        ensure(ores_native_vm_await_async_send(
            vm, ORES_NATIVE_VM_MAIN, ticket, &outcome));
        ensure(ores_native_vm_release_async_send(
            vm, ORES_NATIVE_VM_MAIN, ticket));
    }

    group_lock();
    ensure(instance->magic == SOURCE_ACTOR_MAGIC
        && instance->async_waiters > 0);
    --instance->async_waiters;
    ensure(pthread_cond_broadcast(&group.async_waiters_changed) == 0);
    group_unlock();
    return (int)outcome;
}

static int source_send_future_valid_locked(SourceSendFuture* future) {
    /*
     * Opaque host pointers are capabilities, but a checked API must not
     * dereference an arbitrary pointer merely to decide it is invalid. Prove
     * membership in one currently-live actor wrapper pool first, then inspect
     * wrapper state. Equality comparison itself does not access foreign memory.
     */
    if (!future || !group.vm) return 0;
    for (size_t slot = 0; slot < group.next_slot; ++slot) {
        SourceActor* actor = group.actors[slot];
        if (!actor || actor->magic != SOURCE_ACTOR_MAGIC) continue;
        for (size_t i = 0; i < ORES_SOURCE_SEND_ASYNC_MAX_WAITERS; ++i) {
            if (future != &actor->send_futures[i]) continue;
            return future->live && future->actor == actor
                && actor->ref.actor_slot == slot;
        }
    }
    return 0;
}

static void retire_consuming_send_future_terminal(
        SourceSendFuture* future, SourceActor* actor) {
    group_lock();
    ensure(source_send_future_valid_locked(future)
        && future->consuming && future->actor == actor
        && actor->send_future_live > 0);
    OresNativeVM* vm = group.vm;
    const OresNativeAsyncSendTicket ticket = future->ticket;
    group_unlock();

    /* External-host finalization only. Scheduler release rejects every actor
     * carrier so descriptor identity cannot disappear while a wake callback
     * is still executing on its stack. */
    ensure(!ores_native_actor_scheduler_is_carrier_thread());
    ensure(ores_native_vm_release_async_send(
        vm, ORES_NATIVE_VM_MAIN, ticket));

    group_lock();
    ensure(source_send_future_valid_locked(future)
        && future->consuming && future->actor == actor
        && actor->send_future_live > 0);
    future->live = 0;
    future->actor = NULL;
    future->ticket = (OresNativeAsyncSendTicket){0};
    --actor->send_future_live;
    ensure(pthread_cond_broadcast(&group.async_waiters_changed) == 0);
    group_unlock();
}

static void abandon_consuming_send_future_unbound(
        SourceSendFuture* future, SourceActor* actor) {
    ensure(!ores_native_actor_scheduler_is_carrier_thread());
    group_lock();
    ensure(source_send_future_valid_locked(future)
        && future->consuming && future->actor == actor
        && actor->send_future_live > 0);
    OresNativeVM* vm = group.vm;
    const OresNativeAsyncSendTicket ticket = future->ticket;
    group_unlock();

    if (!ores_native_vm_cancel_async_send(
            vm, ORES_NATIVE_VM_MAIN, ticket)) {
        OresNativeAsyncSendState state = ORES_NATIVE_ASYNC_SEND_INVALID;
        OresNativeSendStatus outcome = ORES_NATIVE_SEND_INVALID_REF;
        ensure(ores_native_vm_async_send_status(
            vm, ORES_NATIVE_VM_MAIN, ticket, &state, &outcome));
        ensure(state == ORES_NATIVE_ASYNC_SEND_READY
            || state == ORES_NATIVE_ASYNC_SEND_FAILED
            || state == ORES_NATIVE_ASYNC_SEND_CANCELLED);
    }
    ensure(ores_native_vm_release_async_send(
        vm, ORES_NATIVE_VM_MAIN, ticket));

    group_lock();
    ensure(source_send_future_valid_locked(future)
        && future->consuming && future->actor == actor
        && actor->send_future_live > 0);
    future->live = 0;
    future->actor = NULL;
    future->ticket = (OresNativeAsyncSendTicket){0};
    --actor->send_future_live;
    ensure(pthread_cond_broadcast(&group.async_waiters_changed) == 0);
    group_unlock();
}

int oreslang_source_actor_send_async_start_checked(
        void* opaque, int64_t payload, void** future_out) {
    if (future_out) *future_out = NULL;
    SourceActor* instance = (SourceActor*)opaque;
    if (!instance || !future_out) return ORES_SOURCE_SEND_ASYNC_INVALID_REF;

    group_lock();
    if (instance->magic != SOURCE_ACTOR_MAGIC || !group.vm
            || !instance->on_message
            || instance->ref.actor_slot >= group.next_slot
            || group.actors[instance->ref.actor_slot] != instance) {
        group_unlock();
        return ORES_SOURCE_SEND_ASYNC_INVALID_REF;
    }
    /*
     * Source stop and new send_async submission share group.mutex. Once STOP
     * wins, do not mint a new Future wrapper or scheduler descriptor while
     * physical actor retirement is still in progress.
     */
    if (atomic_load_explicit(
            &instance->stop_requested, memory_order_acquire)) {
        group_unlock();
        return ORES_SOURCE_SEND_ASYNC_TERMINATED;
    }
    if (instance->send_future_live >= ORES_SOURCE_SEND_ASYNC_MAX_WAITERS) {
        group_unlock();
        return ORES_SOURCE_SEND_ASYNC_RESOURCE_EXHAUSTED;
    }

    SourceSendFuture* future = NULL;
    for (size_t i = 0; i < ORES_SOURCE_SEND_ASYNC_MAX_WAITERS; ++i) {
        /*
         * The source ABI exposes an opaque pointer, not a (slot,generation)
         * pair. Never recycle a wrapper address within one actor lifetime:
         * otherwise a stale trusted-host pointer could alias a newer Future.
         * The current compiler admits at most one stored send Future; 256
         * one-shot wrapper capabilities leave ample bounded headroom.
         */
        if (!instance->send_futures[i].live
                && instance->send_futures[i].generation == 0) {
            future = &instance->send_futures[i];
            break;
        }
    }
    if (!future) {
        group_unlock();
        return ORES_SOURCE_SEND_ASYNC_RESOURCE_EXHAUSTED;
    }

    OresNativeVM* vm = group.vm;
    const OresNativeActorRef target = instance->ref;
    OresNativeAsyncSendTicket ticket = {0};
    const OresNativeSendStatus status = ores_native_vm_submit_async(
        vm, ORES_NATIVE_VM_MAIN, target, (uint64_t)payload, &ticket);
    if (status != ORES_NATIVE_SEND_ACCEPTED) {
        group_unlock();
        return (int)status;
    }
    ensure(ticket.generation != 0);
    ++future->generation;
    if (!future->generation) abort();
    future->actor = instance;
    future->ticket = ticket;
    future->live = 1;
    future->consuming = 0;
    ++instance->send_future_live;
    *future_out = future;
    group_unlock();
    return ORES_SOURCE_SEND_ASYNC_ACCEPTED;
}

void* oreslang_source_actor_send_async_start(
        void* opaque, int64_t payload) {
    void* future = NULL;
    ensure(oreslang_source_actor_send_async_start_checked(
        opaque, payload, &future) == ORES_SOURCE_SEND_ASYNC_ACCEPTED);
    ensure(future != NULL);
    return future;
}

int oreslang_source_actor_await_send_future_checked(void* opaque_future) {
    SourceSendFuture* future = (SourceSendFuture*)opaque_future;
    group_lock();
    if (!source_send_future_valid_locked(future) || future->consuming) {
        group_unlock();
        return ORES_SOURCE_SEND_ASYNC_INVALID_REF;
    }
    if (ores_native_actor_scheduler_is_carrier_thread()) {
        group_unlock();
        return ORES_SOURCE_SEND_ASYNC_WOULD_DEADLOCK;
    }
    /* Reserve exclusive terminal ownership before dropping the group lock.
     * Without this claim, concurrent cancel could release the descriptor
     * while await is sleeping and trigger an abort in the host bridge. */
    future->consuming = 1;
    SourceActor* actor = future->actor;
    OresNativeVM* vm = group.vm;
    const OresNativeAsyncSendTicket ticket = future->ticket;
    group_unlock();

    OresNativeSendStatus outcome = ORES_NATIVE_SEND_INVALID_REF;
    ensure(ores_native_vm_await_async_send(
        vm, ORES_NATIVE_VM_MAIN, ticket, &outcome));
    ensure(ores_native_vm_release_async_send(
        vm, ORES_NATIVE_VM_MAIN, ticket));

    group_lock();
    ensure(source_send_future_valid_locked(future)
        && future->actor == actor && actor->send_future_live > 0);
    future->live = 0;
    future->actor = NULL;
    future->ticket = (OresNativeAsyncSendTicket){0};
    --actor->send_future_live;
    ensure(pthread_cond_broadcast(&group.async_waiters_changed) == 0);
    group_unlock();
    return (int)outcome;
}

int oreslang_source_actor_discard_send_future_checked(void* opaque_future) {
    SourceSendFuture* future = (SourceSendFuture*)opaque_future;
    if (!future) return ORES_SOURCE_SEND_ASYNC_INVALID_REF;
    if (ores_native_actor_scheduler_is_carrier_thread())
        return ORES_SOURCE_SEND_ASYNC_WOULD_DEADLOCK;

    group_lock();
    if (!source_send_future_valid_locked(future) || future->consuming) {
        group_unlock();
        return ORES_SOURCE_SEND_ASYNC_INVALID_REF;
    }
    future->consuming = 1;
    SourceActor* actor = future->actor;
    OresNativeVM* vm = group.vm;
    const OresNativeAsyncSendTicket ticket = future->ticket;
    group_unlock();

    OresNativeSendStatus outcome = ORES_NATIVE_SEND_CANCELLED;
    if (!ores_native_vm_cancel_async_send(
            vm, ORES_NATIVE_VM_MAIN, ticket)) {
        outcome = ORES_NATIVE_SEND_INVALID_REF;
        ensure(ores_native_vm_await_async_send(
            vm, ORES_NATIVE_VM_MAIN, ticket, &outcome));
    }
    ensure(ores_native_vm_release_async_send(
        vm, ORES_NATIVE_VM_MAIN, ticket));

    group_lock();
    ensure(source_send_future_valid_locked(future)
        && future->consuming && future->actor == actor
        && actor->send_future_live > 0);
    future->live = 0;
    future->actor = NULL;
    future->ticket = (OresNativeAsyncSendTicket){0};
    --actor->send_future_live;
    ensure(pthread_cond_broadcast(&group.async_waiters_changed) == 0);
    group_unlock();

#ifdef ORESLANG_SOURCE_ACTOR_TESTING
    fprintf(stderr,
        "SOURCE_SEND_ASYNC_DISCARD_PASS outcome=%d consumed=1\n",
        (int)outcome);
#endif
    return (int)outcome;
}

int oreslang_source_actor_await_send_future_continuation_checked(
        void* opaque_future,
        OresSourceActorContinuation continuation,
        uint64_t source_token,
        int64_t* result_out) {
    if (result_out) *result_out = 0;
    SourceSendFuture* future = (SourceSendFuture*)opaque_future;
    if (!future || !continuation || !result_out)
        return ORES_SOURCE_SEND_ASYNC_INVALID_REF;
    if (ores_native_actor_scheduler_is_carrier_thread())
        return ORES_SOURCE_SEND_ASYNC_WOULD_DEADLOCK;

    SendAwaitFrame frame = {
        .continuation = continuation,
        .send_future = future,
        .source_token = source_token,
        .outcome = ORES_SOURCE_SEND_ASYNC_INVALID_REF
    };
    ensure(pthread_cond_init(&frame.completed, NULL) == 0);

    group_lock();
    if (!source_send_future_valid_locked(future) || future->consuming) {
        group_unlock();
        ensure(pthread_cond_destroy(&frame.completed) == 0);
        return ORES_SOURCE_SEND_ASYNC_INVALID_REF;
    }
    if (!group.root_ref.scheduler_domain
            || group.pending_ready != NULL
            || group.pending_done != NULL
            || group.pending_send != NULL) {
        group_unlock();
        ensure(pthread_cond_destroy(&frame.completed) == 0);
        return ORES_SOURCE_SEND_ASYNC_RESOURCE_EXHAUSTED;
    }

    SourceActor* actor = future->actor;
    OresNativeVM* vm = group.vm;
    const OresNativeActorRef root_ref = group.root_ref;
    const OresNativeAsyncSendTicket ticket = future->ticket;
    frame.actor = actor;
    future->consuming = 1;
    group.pending_send = &frame;

    /* Capture the logical suspension before binding scheduler settlement.
     * Once suspend_recorded is visible, even an already-READY send is resumed
     * on a fresh authenticated root wake turn rather than this host stack. */
    if (!ores_native_vm_publish_control(
            vm,
            ORES_NATIVE_VM_MAIN,
            root_ref,
            SOURCE_ROOT_CONTROL_SEND_SUSPEND)) {
        group.pending_send = NULL;
        future->consuming = 0;
        group_unlock();
        ensure(pthread_cond_destroy(&frame.completed) == 0);
        return ORES_SOURCE_SEND_ASYNC_RESOURCE_EXHAUSTED;
    }
    while (!frame.suspend_recorded)
        ensure(pthread_cond_wait(&frame.completed, &group.mutex) == 0);
    group_unlock();

    OresNativeWakeTicket wake = {0};
    if (!ores_native_vm_bind_async_send_wake(
            vm,
            ORES_NATIVE_VM_MAIN,
            ticket,
            root_ref,
            frame.link.continuation.slot,
            frame.link.continuation.generation,
            1,
            &wake)) {
        /* Wake reservation failure does not consume the source Future. Retire
         * only this abandoned logical suspension and release the observer claim. */
        group_lock();
        ensure(group.vm == vm && group.pending_send == &frame
            && source_send_future_valid_locked(future)
            && future->actor == actor && future->consuming);
        ensure(ores_native_continuation_store_cancel(
            group.continuations, frame.link.continuation));
        ensure(ores_native_continuation_store_release(
            group.continuations, frame.link.continuation));
        group.pending_send = NULL;
        future->consuming = 0;
        ensure(ores_native_execution_context_store_live(group.executions) == 0);
        ensure(ores_native_continuation_store_live(group.continuations) == 0);
        group_unlock();
        ensure(pthread_cond_destroy(&frame.completed) == 0);
        return ORES_SOURCE_SEND_ASYNC_RESOURCE_EXHAUSTED;
    }

    /* This compatibility ABI remains synchronous to its trusted outer caller,
     * but mailbox settlement itself no longer parks or polls that caller:
     * the scheduler publishes the producer wake directly. */
    group_lock();
    while (!frame.done)
        ensure(pthread_cond_wait(&frame.completed, &group.mutex) == 0);

    ensure(group.pending_send == &frame);
    group.pending_send = NULL;
    const int outcome = frame.outcome;
    const int64_t result = frame.result;
    ensure(ores_native_continuation_store_release(
        group.continuations, frame.link.continuation));
    ensure(ores_native_execution_context_store_live(group.executions) == 0);
    ensure(ores_native_continuation_store_live(group.continuations) == 0);
    group_unlock();

    ensure(ores_native_vm_release_async_send(
        vm, ORES_NATIVE_VM_MAIN, ticket));

    group_lock();
    ensure(source_send_future_valid_locked(future)
        && future->actor == actor && actor->send_future_live > 0);
    future->live = 0;
    future->actor = NULL;
    future->ticket = (OresNativeAsyncSendTicket){0};
    --actor->send_future_live;
    ensure(pthread_cond_broadcast(&group.async_waiters_changed) == 0);
    group_unlock();

    ensure(pthread_cond_destroy(&frame.completed) == 0);
#ifdef ORESLANG_SOURCE_ACTOR_TESTING
    fprintf(stderr,
        "SOURCE_AWAIT_SEND_FUTURE_TRACE_PASS outcome=%d execution_id_persisted=1 fresh_resume=1 scheduler_wake=1 host_settlement_observer=0 root_slot=%u source_token=%llu\n",
        outcome,
        SOURCE_GROUP_ROOT_SLOT,
        (unsigned long long)source_token);
#endif
    *result_out = result;
    return outcome;
}

int64_t oreslang_source_actor_await_send_future(
        void* opaque_future,
        OresSourceActorContinuation continuation,
        uint64_t source_token) {
    int64_t result = 0;
    ensure(oreslang_source_actor_await_send_future_continuation_checked(
        opaque_future, continuation, source_token, &result)
        == ORES_SOURCE_SEND_ASYNC_ACCEPTED);
    return result;
}

static int detach_send_future_impl(
        void* opaque_future,
        OresSourceActorContinuation continuation,
        OresSourceActorStateContinuation state_continuation,
        uint64_t source_token,
        const int64_t* state,
        uint64_t state_count) {
    SourceSendFuture* future = (SourceSendFuture*)opaque_future;
    if (!future || ((continuation != NULL) == (state_continuation != NULL)))
        return ORES_SOURCE_SEND_ASYNC_INVALID_REF;
    if (state_count > ORES_SOURCE_DETACHED_SCALAR_WORDS)
        return ORES_SOURCE_SEND_ASYNC_RESOURCE_EXHAUSTED;
    if ((state_count != 0 && state == NULL)
            || (state_continuation == NULL && state_count != 0))
        return ORES_SOURCE_SEND_ASYNC_INVALID_REF;
    if (ores_native_actor_scheduler_is_carrier_thread())
        return ORES_SOURCE_SEND_ASYNC_WOULD_DEADLOCK;

    group_lock();
    if (!source_send_future_valid_locked(future) || future->consuming) {
        group_unlock();
        return ORES_SOURCE_SEND_ASYNC_INVALID_REF;
    }
    if (!group.root_ref.scheduler_domain
            || group.pending_ready != NULL
            || group.pending_done != NULL
            || group.pending_send != NULL) {
        group_unlock();
        return ORES_SOURCE_SEND_ASYNC_RESOURCE_EXHAUSTED;
    }

    SendAwaitFrame* frame = &group.detached_send;
    *frame = (SendAwaitFrame){
        .continuation = continuation,
        .state_continuation = state_continuation,
        .detached_state_count = state_count,
        .send_future = future,
        .actor = future->actor,
        .source_token = source_token,
        .detached = 1,
        .outcome = ORES_SOURCE_SEND_ASYNC_INVALID_REF
    };
    for (uint64_t i = 0; i < state_count; ++i)
        frame->detached_state[i] = state[i];
    ensure(pthread_cond_init(&frame->completed, NULL) == 0);

    future->consuming = 1;
    group.pending_send = frame;

    /*
     * Detachment is established before publishing root CONTROL. If CONTROL
     * admission itself is unavailable, keep the detached frame terminal so
     * the process entry can still reclaim the Future/actor without reviving
     * this guest stack. Returning ACCEPTED here means "runtime owns the
     * detached frame", not "mailbox admission has succeeded".
     */
    if (!ores_native_vm_publish_control(
            group.vm,
            ORES_NATIVE_VM_MAIN,
            group.root_ref,
            SOURCE_ROOT_CONTROL_SEND_SUSPEND)) {
        frame->outcome = ORES_SOURCE_SEND_ASYNC_RESOURCE_EXHAUSTED;
        frame->done = 1;
        ensure(pthread_cond_signal(&frame->completed) == 0);
    }
    group_unlock();
    return ORES_SOURCE_SEND_ASYNC_ACCEPTED;
}

int oreslang_source_actor_detach_send_future_checked(
        void* opaque_future,
        OresSourceActorContinuation continuation,
        uint64_t source_token) {
    return detach_send_future_impl(
        opaque_future, continuation, NULL, source_token, NULL, 0);
}

int oreslang_source_actor_detach_send_future_state_checked(
        void* opaque_future,
        OresSourceActorStateContinuation continuation,
        uint64_t source_token,
        const int64_t* state,
        uint64_t state_count) {
    return detach_send_future_impl(
        opaque_future, NULL, continuation, source_token, state, state_count);
}

int oreslang_source_actor_rearm_detached_send_state_checked(
        void* opaque_actor,
        int64_t payload,
        OresSourceActorStateContinuation continuation,
        uint64_t source_token,
        const int64_t* state,
        uint64_t state_count) {
    SourceActor* actor = (SourceActor*)opaque_actor;
    if (!actor || !continuation || (state_count != 0 && state == NULL))
        return ORES_SOURCE_SEND_ASYNC_INVALID_REF;
    if (state_count > ORES_SOURCE_DETACHED_SCALAR_WORDS)
        return ORES_SOURCE_SEND_ASYNC_RESOURCE_EXHAUSTED;
    if (!ores_native_actor_scheduler_is_carrier_thread()
            || !ores_native_store_handle_valid(current_source_execution))
        return ORES_SOURCE_SEND_ASYNC_WOULD_DEADLOCK;

    group_lock();
    SendAwaitFrame* frame = group.pending_send;
    if (frame != &group.detached_send || !frame->detached || frame->done
            || frame->rearm_requested
            || frame->actor != actor
            || actor->magic != SOURCE_ACTOR_MAGIC
            || frame->pc + 1u >= ORES_SOURCE_DETACHED_MAX_PCS
            || !ores_native_store_handle_valid(frame->link.continuation)) {
        group_unlock();
        return ORES_SOURCE_SEND_ASYNC_RESOURCE_EXHAUSTED;
    }
    const OresNativeContinuationHandle parent = frame->link.continuation;
    group_unlock();

    void* next_raw = NULL;
    const int submit_status =
        oreslang_source_actor_send_async_start_checked(
            actor, payload, &next_raw);
    if (submit_status != ORES_SOURCE_SEND_ASYNC_ACCEPTED) {
        group_lock();
        ensure(group.pending_send == frame && !frame->done
            && !frame->rearm_requested && frame->actor == actor);
        frame->rearm_requested = 1;
        frame->rearm_terminal = 1;
        frame->rearm_outcome = submit_status;
        group_unlock();
        return submit_status;
    }

    SourceSendFuture* next = (SourceSendFuture*)next_raw;
    OresNativeSuspensionLink next_link = {0};

    group_lock();
    ensure(group.pending_send == frame && !frame->done
        && !frame->rearm_requested && frame->actor == actor);
    ensure(source_send_future_valid_locked(next)
        && !next->consuming && next->actor == actor);
    next->consuming = 1;

    const int suspended = ores_native_await_trace_suspend(
        group.executions,
        group.continuations,
        current_source_execution,
        parent,
        ORES_CONTINUATION_BOUNDARY_AWAIT,
        1,
        source_token,
        &next_link);
    if (!suspended) {
        frame->rearm_requested = 1;
        frame->rearm_terminal = 1;
        frame->rearm_outcome = ORES_SOURCE_SEND_ASYNC_RESOURCE_EXHAUSTED;
        frame->abandoned_send_future = next;
        group_unlock();
        return ORES_SOURCE_SEND_ASYNC_RESOURCE_EXHAUSTED;
    }

    frame->next_send_future = next;
    frame->next_state_continuation = continuation;
    frame->next_source_token = source_token;
    frame->next_state_count = state_count;
    for (uint64_t i = 0; i < ORES_SOURCE_DETACHED_SCALAR_WORDS; ++i)
        frame->next_state[i] = 0;
    for (uint64_t i = 0; i < state_count; ++i)
        frame->next_state[i] = state[i];
    frame->next_link = next_link;
    frame->rearm_requested = 1;
    frame->rearm_terminal = 0;
    frame->rearm_outcome = ORES_SOURCE_SEND_ASYNC_ACCEPTED;
    group_unlock();
    return ORES_SOURCE_SEND_ASYNC_ACCEPTED;
}

int oreslang_source_actor_detach_select_send2_checked(
        void* opaque0, int64_t payload0,
        void* opaque1, int64_t payload1,
        OresSourceSelectSend2Continuation continuation,
        uint64_t source_token) {
    SourceActor* actor0 = (SourceActor*)opaque0;
    SourceActor* actor1 = (SourceActor*)opaque1;
    if (!actor0 || !actor1 || actor0 == actor1 || !continuation)
        return ORES_SOURCE_SEND_ASYNC_INVALID_REF;
    if (ores_native_actor_scheduler_is_carrier_thread())
        return ORES_SOURCE_SEND_ASYNC_WOULD_DEADLOCK;

    group_lock();
    const size_t slot0 = (size_t)actor0->ref.actor_slot;
    const size_t slot1 = (size_t)actor1->ref.actor_slot;
    if (!group.vm
            || actor0->magic != SOURCE_ACTOR_MAGIC
            || actor1->magic != SOURCE_ACTOR_MAGIC
            || slot0 >= group.next_slot || slot1 >= group.next_slot
            || group.actors[slot0] != actor0
            || group.actors[slot1] != actor1) {
        group_unlock();
        return ORES_SOURCE_SEND_ASYNC_INVALID_REF;
    }
    if (atomic_load_explicit(
            &actor0->stop_requested, memory_order_acquire)
            || atomic_load_explicit(
                &actor1->stop_requested, memory_order_acquire)) {
        group_unlock();
        return ORES_SOURCE_SEND_ASYNC_TERMINATED;
    }
    if (!group.root_ref.scheduler_domain
            || group.pending_ready != NULL
            || group.pending_done != NULL
            || group.pending_send != NULL
            || group.pending_select != NULL) {
        group_unlock();
        return ORES_SOURCE_SEND_ASYNC_RESOURCE_EXHAUSTED;
    }

    SelectAwaitFrame* frame = &group.detached_select;
    frame->continuation = continuation;
    frame->actors[0] = actor0;
    frame->actors[1] = actor1;
    frame->payloads[0] = payload0;
    frame->payloads[1] = payload1;
    frame->selection_generation = 0;
    frame->tokens[0] = (OresNativeSelectToken){0};
    frame->tokens[1] = (OresNativeSelectToken){0};
    frame->common_wake = (OresNativeSelectWakeTicket){0};
    frame->sends[0] = (OresNativeAsyncSendTicket){0};
    frame->sends[1] = (OresNativeAsyncSendTicket){0};
    frame->send_live[0] = frame->send_live[1] = 0;
    frame->immediate_status[0] = frame->immediate_status[1] =
        ORES_SOURCE_SEND_ASYNC_INVALID_REF;
    frame->link = (OresNativeSuspensionLink){0};
    frame->source_token = source_token;
    frame->suspend_recorded = 0;
    frame->wake_reserved = 0;
    frame->setup_complete = 0;
    frame->finalizer_claimed = 0;
    frame->done = 0;
    frame->outcome = ORES_SOURCE_SEND_ASYNC_INVALID_REF;
    frame->winner_arm = UINT64_MAX;
    frame->result = 0;
    ensure(pthread_cond_init(&frame->completed, NULL) == 0);

    group.pending_select = frame;

    /* Detach is complete before CONTROL publication. A failure to admit the
     * capture turn remains runtime-owned and is finalized by the outer host. */
    if (!ores_native_vm_publish_control(
            group.vm,
            ORES_NATIVE_VM_MAIN,
            group.root_ref,
            SOURCE_ROOT_CONTROL_SELECT_SUSPEND)) {
        frame->outcome = ORES_SOURCE_SEND_ASYNC_RESOURCE_EXHAUSTED;
        frame->done = 1;
        ensure(pthread_cond_signal(&frame->completed) == 0);
    }
    group_unlock();
    return ORES_SOURCE_SEND_ASYNC_ACCEPTED;
}

int oreslang_source_actor_wait_detached_select_send2_result(
        uint64_t* winner_arm_out, int64_t* result_out) {
    if (winner_arm_out) *winner_arm_out = UINT64_MAX;
    if (result_out) *result_out = 0;
    if (!winner_arm_out || !result_out)
        return ORES_SOURCE_SEND_ASYNC_INVALID_REF;
    if (ores_native_actor_scheduler_is_carrier_thread())
        return ORES_SOURCE_SEND_ASYNC_WOULD_DEADLOCK;

    group_lock();
    SelectAwaitFrame* frame = &group.detached_select;
    if (group.pending_select != frame
            || frame->finalizer_claimed
            || !frame->actors[0] || !frame->actors[1]) {
        group_unlock();
        return ORES_SOURCE_SEND_ASYNC_INVALID_REF;
    }
    frame->finalizer_claimed = 1;
    while (!frame->done)
        ensure(pthread_cond_wait(&frame->completed, &group.mutex) == 0);

    SourceActor* actor0 = frame->actors[0];
    SourceActor* actor1 = frame->actors[1];
    OresNativeVM* vm = group.vm;
    const int outcome = frame->outcome;
    const uint64_t winner_arm = frame->winner_arm;
    const int64_t result = frame->result;
    const int suspended = frame->suspend_recorded;
    const int setup_complete = frame->setup_complete;
    const int wake_reserved = frame->wake_reserved;
    const OresNativeSelectWakeTicket common_wake = frame->common_wake;
    OresNativeAsyncSendTicket sends[2] = {
        frame->sends[0], frame->sends[1]
    };
    int send_live[2] = {
        frame->send_live[0], frame->send_live[1]
    };
    group_unlock();

    /* Descriptors never own the common wake. Cancel any losing still-pending
     * descriptor, observe terminal state if cancellation lost, then release
     * every exact descriptor generation from this external host. */
    for (size_t i = 0; i < 2; ++i) {
        if (!send_live[i]) continue;
        OresNativeAsyncSendState state = ORES_NATIVE_ASYNC_SEND_INVALID;
        OresNativeSendStatus exact = ORES_NATIVE_SEND_INVALID_REF;
        ensure(ores_native_vm_async_send_status(
            vm, ORES_NATIVE_VM_MAIN, sends[i], &state, &exact));
        if (state == ORES_NATIVE_ASYNC_SEND_PENDING) {
            if (!ores_native_vm_cancel_async_send(
                    vm, ORES_NATIVE_VM_MAIN, sends[i])) {
                ensure(ores_native_vm_await_async_send(
                    vm, ORES_NATIVE_VM_MAIN, sends[i], &exact));
            }
        }
        ensure(ores_native_vm_release_async_send(
            vm, ORES_NATIVE_VM_MAIN, sends[i]));
    }

    /* frame->done is set by the source callback before the scheduler epilogue
     * physically clears its wake credit. This exact barrier is what makes it
     * safe to reuse/reset the embedded arbiter memory. */
    if (wake_reserved)
        ensure(ores_native_vm_wait_select_wake_retired(
            vm, ORES_NATIVE_VM_MAIN, common_wake));

    group_lock();
    ensure(group.pending_select == frame);
    if (suspended) {
        if (!setup_complete) {
            /* Root setup failure cancelled the logical continuation but left
             * its frame-owned reference for this single external finalizer. */
            ensure(ores_native_select_active_physical_loans(
                &frame->arbiter) == 0);
        }
        ensure(ores_native_continuation_store_release(
            group.continuations, frame->link.continuation));
    }
    ensure(ores_native_select_active_tokens(&frame->arbiter) == 0);
    ensure(ores_native_select_active_physical_loans(&frame->arbiter) == 0);
    group.pending_select = NULL;

    frame->continuation = NULL;
    frame->actors[0] = frame->actors[1] = NULL;
    frame->common_wake = (OresNativeSelectWakeTicket){0};
    frame->sends[0] = frame->sends[1] = (OresNativeAsyncSendTicket){0};
    frame->send_live[0] = frame->send_live[1] = 0;
    frame->link = (OresNativeSuspensionLink){0};
    frame->source_token = 0;
    frame->selection_generation = 0;
    frame->suspend_recorded = 0;
    frame->wake_reserved = 0;
    frame->setup_complete = 0;
    frame->finalizer_claimed = 0;
    frame->done = 0;
    frame->outcome = ORES_SOURCE_SEND_ASYNC_INVALID_REF;
    frame->winner_arm = UINT64_MAX;
    frame->result = 0;
    group_unlock();

    ensure(pthread_cond_destroy(&frame->completed) == 0);

    /* Guest continuation may have issued nonblocking STOP for either actor.
     * The finalizer supplies missing stop requests and alone performs joins. */
    if (!atomic_load_explicit(
            &actor0->stop_requested, memory_order_acquire))
        oreslang_source_actor_request_stop(actor0);
    if (!atomic_load_explicit(
            &actor1->stop_requested, memory_order_acquire))
        oreslang_source_actor_request_stop(actor1);
    oreslang_source_actor_join_done(actor0);
    oreslang_source_actor_join_done(actor1);

#ifdef ORESLANG_SOURCE_ACTOR_TESTING
    fprintf(stderr,
        "SOURCE_SELECT_SEND2_PASS outcome=%d winner=%llu "
        "common_wake=1 physical_retirement=1 final_join=2\n",
        outcome, (unsigned long long)winner_arm);
#endif
    *winner_arm_out = winner_arm;
    *result_out = result;
    return outcome;
}

int oreslang_source_actor_wait_detached_send_result(int64_t* result_out) {
    if (result_out) *result_out = 0;
    if (!result_out) return ORES_SOURCE_SEND_ASYNC_INVALID_REF;
    if (ores_native_actor_scheduler_is_carrier_thread())
        return ORES_SOURCE_SEND_ASYNC_WOULD_DEADLOCK;

    group_lock();
    SendAwaitFrame* frame = &group.detached_send;
    if (group.pending_send != frame || !frame->detached
            || frame->finalizer_claimed
            || !frame->send_future || !frame->actor) {
        group_unlock();
        return ORES_SOURCE_SEND_ASYNC_INVALID_REF;
    }
    /* Runtime ownership may have been detached from the guest stack, but
     * terminal finalization itself is still linear: exactly one external host
     * may consume the frame, descriptor, Future wrapper, and final actor join. */
    frame->finalizer_claimed = 1;
    while (!frame->done)
        ensure(pthread_cond_wait(&frame->completed, &group.mutex) == 0);

    SourceSendFuture* future = frame->send_future;
    SourceActor* actor = frame->actor;
    OresNativeVM* vm = group.vm;
    const OresNativeAsyncSendTicket ticket = future->ticket;
    const int outcome = frame->outcome;
    const int64_t result = frame->result;
    const int suspended = frame->suspend_recorded;
    const int wake_bound = frame->wake_bound;

    ensure(source_send_future_valid_locked(future)
        && future->consuming && future->actor == actor);
    ensure(frame->completed_send_count < ORES_SOURCE_DETACHED_MAX_PCS);
    for (uint32_t i = 0; i < frame->completed_send_count; ++i) {
        SourceSendFuture* completed = frame->completed_send_futures[i];
        ensure(completed && completed != future
            && source_send_future_valid_locked(completed)
            && completed->consuming && completed->actor == actor);
    }
    if (frame->abandoned_send_future) {
        ensure(frame->abandoned_send_future != future
            && source_send_future_valid_locked(frame->abandoned_send_future)
            && frame->abandoned_send_future->consuming
            && frame->abandoned_send_future->actor == actor);
    }

    /*
     * A failed root CONTROL admission never minted a logical continuation.
     * A later descriptor-bind failure did; retire that still-PENDING logical
     * continuation explicitly. Successful/failed scheduler wakes already ran
     * trace resume/finish before setting frame.done.
     */
    if (suspended) {
        if (!wake_bound)
            ensure(ores_native_continuation_store_cancel(
                group.continuations, frame->link.continuation));
        ensure(ores_native_continuation_store_release(
            group.continuations, frame->link.continuation));
    }
    /*
     * Keep pending_send pinned to this embedded frame through descriptor and
     * Future retirement. A competing trusted-host detach must not reinitialize
     * its pthread_cond_t while this finalizer still owns the old frame.
     */
    ensure(group.pending_send == frame);
    const uint32_t completed_send_count = frame->completed_send_count;
    SourceSendFuture* completed_send_futures[
        ORES_SOURCE_DETACHED_MAX_PCS - 1] = {0};
    for (uint32_t i = 0; i < completed_send_count; ++i)
        completed_send_futures[i] = frame->completed_send_futures[i];
    SourceSendFuture* abandoned_send_future = frame->abandoned_send_future;
    group_unlock();

    for (uint32_t i = 0; i < completed_send_count; ++i)
        retire_consuming_send_future_terminal(
            completed_send_futures[i], actor);
    if (abandoned_send_future)
        abandon_consuming_send_future_unbound(
            abandoned_send_future, actor);

    if (!wake_bound)
        (void)ores_native_vm_cancel_async_send(
            vm, ORES_NATIVE_VM_MAIN, ticket);
    ensure(ores_native_vm_release_async_send(
        vm, ORES_NATIVE_VM_MAIN, ticket));

    group_lock();
    ensure(group.pending_send == frame);
    ensure(source_send_future_valid_locked(future)
        && future->consuming && future->actor == actor
        && actor->send_future_live > 0);
    future->live = 0;
    future->actor = NULL;
    future->ticket = (OresNativeAsyncSendTicket){0};
    --actor->send_future_live;
    ensure(pthread_cond_broadcast(&group.async_waiters_changed) == 0);
    group_unlock();

    ensure(pthread_cond_destroy(&frame->completed) == 0);

    group_lock();
    ensure(group.pending_send == frame);
    group.pending_send = NULL;
    *frame = (SendAwaitFrame){0};
    group_unlock();

    /*
     * Successful compiler continuations request STOP themselves after all
     * post-await effects. Failure continuations deliberately skip guest code,
     * so outer-host finalization supplies the nonblocking stop request before
     * joining physical completion.
     */
    if (!atomic_load_explicit(
            &actor->stop_requested, memory_order_acquire))
        oreslang_source_actor_request_stop(actor);
    oreslang_source_actor_join_done(actor);

#ifdef ORESLANG_SOURCE_ACTOR_TESTING
    fprintf(stderr,
        "SOURCE_DETACHED_SEND_FUTURE_PASS outcome=%d guest_stack_released=1 host_settlement_observer=0 final_join=1\n",
        outcome);
#endif
    *result_out = result;
    return outcome;
}

int oreslang_source_actor_cancel_send_future(void* opaque_future) {
    SourceSendFuture* future = (SourceSendFuture*)opaque_future;
    group_lock();
    if (!source_send_future_valid_locked(future) || future->consuming) {
        group_unlock();
        return 0;
    }
    /* Cancellation and observation are mutually exclusive even while the
     * scheduler's cancellation API runs outside the source-group lock. */
    future->consuming = 1;
    SourceActor* actor = future->actor;
    OresNativeVM* vm = group.vm;
    const OresNativeAsyncSendTicket ticket = future->ticket;
    group_unlock();

    if (!ores_native_vm_cancel_async_send(
            vm, ORES_NATIVE_VM_MAIN, ticket)) {
        group_lock();
        ensure(source_send_future_valid_locked(future)
            && future->consuming && future->actor == actor);
        future->consuming = 0;
        group_unlock();
        return 0;
    }
    ensure(ores_native_vm_release_async_send(
        vm, ORES_NATIVE_VM_MAIN, ticket));

    group_lock();
    ensure(source_send_future_valid_locked(future)
        && future->actor == actor && actor->send_future_live > 0);
    future->live = 0;
    future->actor = NULL;
    future->ticket = (OresNativeAsyncSendTicket){0};
    --actor->send_future_live;
    ensure(pthread_cond_broadcast(&group.async_waiters_changed) == 0);
    group_unlock();
    return 1;
}

void oreslang_source_actor_await_send_async(void* opaque, int64_t payload) {
    ensure(oreslang_source_actor_await_send_async_checked(opaque, payload)
        == ORES_SOURCE_SEND_ASYNC_ACCEPTED);
}

int oreslang_source_actor_await_ready_checked(
        void* opaque,
        OresSourceActorContinuation continuation,
        uint64_t source_token,
        int64_t* result_out) {
    SourceActor* instance = (SourceActor*)opaque;
    ensure(instance && instance->magic == SOURCE_ACTOR_MAGIC);
    ensure(continuation != NULL && result_out != NULL);

    /*
     * This outer wait is trusted top-level ABI glue, never guest-carrier
     * blocking. A carrier call would be a deadlock/semantic violation.
     */
    OresNativeExecutionContext carrier = {0};
    ensure(!ores_native_vm_current_execution_context(
        group.vm, ORES_NATIVE_VM_MAIN, &carrier));

    ReadyAwaitFrame frame = {
        .continuation = continuation,
        .actor = instance,
        .source_token = source_token
    };
    ensure(pthread_cond_init(&frame.completed, NULL) == 0);

    group_lock();
    ensure(group.vm && group.root_ref.scheduler_domain
        && group.pending_ready == NULL && group.pending_done == NULL
        && group.pending_send == NULL
        && group.pending_select == NULL);
    frame.ready_future = ores_native_vm_actor_future_open(
        group.vm,
        ORES_NATIVE_VM_MAIN,
        group.root_ref,
        SOURCE_ROOT_CONTROL_RESUME);
    ensure(frame.ready_future != NULL);
    group.pending_ready = &frame;

    /*
     * First root turn captures/suspends its scheduler-minted ExecutionId.
     * Future settlement cannot resume until this CONTROL turn has published
     * the OresNativeSuspensionLink.
     */
    ensure(ores_native_vm_publish_control(
        group.vm,
        ORES_NATIVE_VM_MAIN,
        group.root_ref,
        SOURCE_ROOT_CONTROL_SUSPEND));

    while (!frame.done)
        ensure(pthread_cond_wait(&frame.completed, &group.mutex) == 0);

    ensure(group.pending_ready == &frame);
    group.pending_ready = NULL;
    const int outcome = frame.outcome;
    if (outcome == ORES_SOURCE_ACTOR_STARTUP_READY)
        *result_out = frame.result;

    ores_native_actor_future_destroy(frame.ready_future);
    ensure(ores_native_continuation_store_release(
        group.continuations, frame.link.continuation));
    ensure(ores_native_execution_context_store_live(group.executions) == 0);
    ensure(ores_native_continuation_store_live(group.continuations) == 0);
    group_unlock();

    ensure(pthread_cond_destroy(&frame.completed) == 0);
#ifdef ORESLANG_SOURCE_ACTOR_TESTING
    fprintf(stderr,
        "SOURCE_AWAIT_READY_TRACE_PASS outcome=%d execution_id_persisted=1 fresh_resume=1 root_slot=%u source_token=%llu\n",
        outcome,
        SOURCE_GROUP_ROOT_SLOT,
        (unsigned long long)source_token);
#endif
    return outcome;
}

int64_t oreslang_source_actor_await_ready(
        void* opaque,
        OresSourceActorContinuation continuation,
        uint64_t source_token) {
    int64_t result = 0;
    const int outcome = oreslang_source_actor_await_ready_checked(
        opaque, continuation, source_token, &result);
    /*
     * Existing source lowering has no typed ActorTerminated projection yet.
     * Fail closed instead of hanging or running the continuation after failed
     * readiness; checked host/runtime callers can observe the failure exactly.
     */
    ensure(outcome == ORES_SOURCE_ACTOR_STARTUP_READY);
    return result;
}

int64_t oreslang_source_actor_await_done(
        void* opaque,
        OresSourceActorContinuation continuation,
        uint64_t source_token) {
    SourceActor* instance = (SourceActor*)opaque;
    ensure(instance && instance->magic == SOURCE_ACTOR_MAGIC);
    ensure(continuation != NULL);
    ensure(atomic_load_explicit(
        &instance->stop_requested, memory_order_acquire) == 1);
    ensure(atomic_load_explicit(
        &instance->join_started, memory_order_acquire) == 0);

    /*
     * The physical wait is trusted top-level glue. It must never park an
     * actor carrier: the target actor may need that carrier to drain accepted
     * work and publish its terminal acknowledgement.
     */
    OresNativeExecutionContext carrier = {0};
    ensure(!ores_native_vm_current_execution_context(
        group.vm, ORES_NATIVE_VM_MAIN, &carrier));

    DoneAwaitFrame frame = {
        .continuation = continuation,
        .actor = instance,
        .source_token = source_token
    };
    ensure(pthread_cond_init(&frame.completed, NULL) == 0);

    group_lock();
    ensure(group.vm && group.root_ref.scheduler_domain
        && group.pending_ready == NULL && group.pending_done == NULL
        && group.pending_send == NULL
        && group.pending_select == NULL);
    OresNativeVM* vm = group.vm;
    const OresNativeActorRef target = instance->ref;
    frame.done_future = ores_native_vm_actor_future_open(
        vm,
        ORES_NATIVE_VM_MAIN,
        group.root_ref,
        SOURCE_ROOT_CONTROL_DONE_RESUME);
    ensure(frame.done_future != NULL);
    group.pending_done = &frame;

    /*
     * Persist logical ancestry on a root CONTROL turn before waiting on the
     * target. The host waits for this acknowledgement so no continuation is
     * completed before its durable suspension link exists.
     */
    ensure(ores_native_vm_publish_control(
        vm,
        ORES_NATIVE_VM_MAIN,
        group.root_ref,
        SOURCE_ROOT_CONTROL_DONE_SUSPEND));
    while (!frame.suspend_recorded)
        ensure(pthread_cond_wait(&frame.completed, &group.mutex) == 0);
    group_unlock();

    ensure(ores_native_vm_wait_done(
        vm, ORES_NATIVE_VM_MAIN, target));

    group_lock();
    ensure(group.vm == vm && group.pending_done == &frame);
    ensure(ores_native_vm_poll_done(
        vm, ORES_NATIVE_VM_MAIN, target));
    ensure(ores_native_actor_future_status(frame.done_future)
        == ORES_NATIVE_FUTURE_PENDING);
    ensure(ores_native_actor_future_complete(
        frame.done_future, ORES_NATIVE_FUTURE_READY));

    while (!frame.done)
        ensure(pthread_cond_wait(&frame.completed, &group.mutex) == 0);

    ensure(group.pending_done == &frame);
    group.pending_done = NULL;
    const int64_t result = frame.result;

    ores_native_actor_future_destroy(frame.done_future);
    ensure(ores_native_continuation_store_release(
        group.continuations, frame.link.continuation));
    ensure(ores_native_execution_context_store_live(group.executions) == 0);
    ensure(ores_native_continuation_store_live(group.continuations) == 0);
    group_unlock();

    ensure(pthread_cond_destroy(&frame.completed) == 0);
#ifdef ORESLANG_SOURCE_ACTOR_TESTING
    fprintf(stderr,
        "SOURCE_AWAIT_DONE_TRACE_PASS terminal_ack=1 execution_id_persisted=1 fresh_resume=1 root_slot=%u source_token=%llu\n",
        SOURCE_GROUP_ROOT_SLOT,
        (unsigned long long)source_token);
#endif

    /*
     * Reclamation is deliberately after the resumed continuation. This keeps
     * the opaque ActorRef allocation valid while the continuation is active,
     * but no guest message/stop capability is admitted after get_done.
     */
    oreslang_source_actor_join_done(instance);
    return result;
}

int oreslang_source_actor_current_execution_handle(
        OresNativeExecutionHandle* out) {
    if (!out || !ores_native_store_handle_valid(current_source_execution))
        return 0;
    *out = current_source_execution;
    return 1;
}

int oreslang_source_actor_snapshot_current_trace(
        OresNativeExecutionTraceFrame* execution_frames,
        size_t execution_capacity,
        OresNativeContinuationTraceFrame* continuation_frames,
        size_t continuation_capacity,
        OresNativeLogicalTraceResult* out) {
    if (!out || !ores_native_store_handle_valid(current_source_execution)
            || !group.executions || !group.continuations)
        return 0;
    return ores_native_logical_trace_snapshot(
        group.executions,
        group.continuations,
        current_source_execution,
        execution_frames,
        execution_capacity,
        continuation_frames,
        continuation_capacity,
        out);
}

void oreslang_source_actor_request_stop(void* opaque) {
    SourceActor* instance = (SourceActor*)opaque;
    ensure(instance && instance->magic == SOURCE_ACTOR_MAGIC);

    /*
     * Stop admission shares the group mutex with startup/readiness publication
     * so exactly one side wins the stop-vs-ready linearization point.
     * This phase still never waits for physical completion.
     */
    group_lock();
    int expected = 0;
    ensure(atomic_compare_exchange_strong_explicit(
        &instance->stop_requested,
        &expected,
        1,
        memory_order_acq_rel,
        memory_order_acquire));

    const size_t slot = (size_t)instance->ref.actor_slot;
    ensure(group.vm && slot < group.next_slot
        && group.actors[slot] == instance);
    /*
     * Source stop matches Java ActorRef.stop(), not gracefulStop(): reject
     * future DATA and abandon queued DATA, while an already-active turn is
     * allowed to cross its physical execution boundary.
     */
    ensure(ores_native_vm_stop_sealed(
        group.vm, ORES_NATIVE_VM_MAIN, instance->ref));
    group_unlock();
}

void oreslang_source_actor_join_done(void* opaque) {
    SourceActor* instance = (SourceActor*)opaque;
    ensure(instance && instance->magic == SOURCE_ACTOR_MAGIC);
    ensure(atomic_load_explicit(
        &instance->stop_requested, memory_order_acquire) == 1);

    int expected = 0;
    ensure(atomic_compare_exchange_strong_explicit(
        &instance->join_started,
        &expected,
        1,
        memory_order_acq_rel,
        memory_order_acquire));

    /*
     * This is trusted outer-host glue, never a carrier turn. Do not block a
     * source actor carrier while waiting for the actor's physical terminal ACK.
     */
    OresNativeExecutionContext carrier = {0};
    ensure(!ores_native_vm_current_execution_context(
        group.vm, ORES_NATIVE_VM_MAIN, &carrier));

    const size_t slot = (size_t)instance->ref.actor_slot;
    ensure(ores_native_vm_wait_done(
        group.vm, ORES_NATIVE_VM_MAIN, instance->ref));

    group_lock();
    ensure(group.vm && slot < group.next_slot
        && group.actors[slot] == instance);
    while (instance->async_waiters != 0
            || instance->send_future_live != 0) {
#ifdef ORESLANG_SOURCE_ACTOR_TESTING
        atomic_store_explicit(
            &test_join_waiting_actor, (uintptr_t)instance,
            memory_order_release);
#endif
        ensure(pthread_cond_wait(
            &group.async_waiters_changed, &group.mutex) == 0);
    }
#ifdef ORESLANG_SOURCE_ACTOR_TESTING
    atomic_store_explicit(
        &test_join_waiting_actor, (uintptr_t)0, memory_order_release);
#endif
    const int startup_state = atomic_load_explicit(
        &instance->startup_state, memory_order_acquire);
    ensure(startup_state == ORES_SOURCE_ACTOR_STARTUP_READY
        || startup_state
            == ORES_SOURCE_ACTOR_STARTUP_TERMINATED_BEFORE_READY);
    ensure(ores_native_vm_poll_done(
        group.vm, ORES_NATIVE_VM_MAIN, instance->ref));

    OresNativeActorTerminalState terminal = {0};
    ensure(ores_native_vm_actor_terminal_state(
        group.vm, ORES_NATIVE_VM_MAIN, instance->ref, &terminal));
    ensure(terminal.kind == ORES_NATIVE_TERMINATION_STOPPED);
    ensure(terminal.processed_user_messages
        + terminal.abandoned_user_messages
        == terminal.accepted_user_messages);
    ensure(terminal.processed_user_messages
        == atomic_load_explicit(&instance->processed, memory_order_acquire));
    ensure(terminal.abandoned_control_messages == 0);

    /*
     * get_done parity requires physical quiescence, not a logical stop bit.
     * Reclaim the actor-owned heap only after the VM terminal acknowledgement.
     */
    ores_actor_i64_heap_retire(instance->heap);
    ensure(ores_actor_i64_heap_try_destroy(instance->heap));
#ifdef ORESLANG_SOURCE_ACTOR_TESTING
    fprintf(stderr,
        "SOURCE_ACTOR_SPAWN_STOP_VERIFIED constructor_on_worker=%d released=1 ready=%d accepted=%llu processed=%llu abandoned=%llu last=%lld\n",
        atomic_load_explicit(
            &instance->constructor_started, memory_order_acquire),
        startup_state == ORES_SOURCE_ACTOR_STARTUP_READY,
        (unsigned long long)terminal.accepted_user_messages,
        (unsigned long long)terminal.processed_user_messages,
        (unsigned long long)terminal.abandoned_user_messages,
        (long long)atomic_load_explicit(
            &instance->last_result, memory_order_relaxed));
#endif
    group.actors[slot] = NULL;
    instance->magic = 0;
    free(instance);
    ensure(group.live > 0);
    group.live--;

    if (group.live == 0) {
        ensure(group.pending_ready == NULL && group.pending_done == NULL
            && group.pending_send == NULL
        && group.pending_select == NULL);
        ensure(ores_native_execution_context_store_live(group.executions) == 0);
        ensure(ores_native_continuation_store_live(group.continuations) == 0);

        ensure(ores_native_vm_close(group.vm));
        group.vm = NULL;
        group.root_ref = (OresNativeActorRef){0, 0, 0};

        ensure(ores_native_continuation_store_close(group.continuations));
        group.continuations = NULL;
        ensure(ores_native_execution_context_store_close(group.executions));
        group.executions = NULL;
#ifdef ORESLANG_SOURCE_ACTOR_TESTING
        fprintf(stderr,
            "SOURCE_ACTOR_GROUP_VERIFIED active=0 slots=%zu peak=%zu carriers=%u\n",
            group.next_slot, group.peak, SOURCE_GROUP_CARRIERS);
#endif
        group.next_slot = 0;
        group.peak = 0;
    }
    group_unlock();
}

void oreslang_source_actor_stop(void* opaque) {
    /*
     * Preserve synchronous stop for source paths that do not await get_done.
     * get_done lowering instead emits request_stop followed by await_done.
     */
    oreslang_source_actor_request_stop(opaque);
    oreslang_source_actor_join_done(opaque);
}
