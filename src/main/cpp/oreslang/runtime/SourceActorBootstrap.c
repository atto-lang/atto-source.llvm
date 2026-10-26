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
#define SOURCE_ROOT_CONTROL_SEND_RESUME UINT64_C(6)

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
    OresNativeActorFuture* resume_future;
    OresSourceActorContinuation continuation;
    SourceSendFuture* send_future;
    SourceActor* actor;
    OresNativeSuspensionLink link;
    uint64_t source_token;
    int suspend_recorded;
    int done;
    int outcome;
    int64_t result;
} SendAwaitFrame;

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
static _Thread_local OresNativeExecutionHandle current_source_execution;

/* The test-only admission hook below uses these helpers before their
 * definitions. C11 requires declarations before first use, including when
 * ORESLANG_SOURCE_ACTOR_TESTING is enabled under -Werror. */
static void group_lock(void);
static void group_unlock(void);

#ifdef ORESLANG_SOURCE_ACTOR_TESTING
static atomic_int test_hold_initialization;
static atomic_int test_initialization_entered;
static atomic_int test_hold_readiness_publication;
static atomic_int test_readiness_publication_entered;

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
    ensure(pthread_cond_signal(&frame->completed) == 0);
    group_unlock();
}

static void root_send_resume_turn(void) {
    SendAwaitFrame* frame;
    OresSourceActorContinuation continuation;
    SourceActor* actor;
    OresNativeSuspensionLink link;
    int future_status;
    int send_outcome;

    group_lock();
    frame = group.pending_send;
    ensure(frame && frame->suspend_recorded && !frame->done);
    ensure(frame->resume_future != NULL);
    future_status = ores_native_actor_future_status(frame->resume_future);
    ensure(future_status == ORES_NATIVE_FUTURE_READY
        || future_status == ORES_NATIVE_FUTURE_FAILED);
    continuation = frame->continuation;
    actor = frame->actor;
    link = frame->link;
    send_outcome = frame->outcome;
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
    if (future_status == ORES_NATIVE_FUTURE_READY) {
        ensure(send_outcome == ORES_SOURCE_SEND_ASYNC_ACCEPTED);
        ensure(!ores_native_store_handle_valid(current_source_execution));
        current_source_execution = resumed;
        result = continuation(actor);
        current_source_execution = (OresNativeExecutionHandle){0, 0};
    } else {
        ensure(send_outcome != ORES_SOURCE_SEND_ASYNC_ACCEPTED);
    }

    ensure(ores_native_await_trace_resume_finish(group.continuations, link));
    ensure(ores_native_execution_context_store_release(
        group.executions, resumed));

    group_lock();
    ensure(frame == group.pending_send && !frame->done);
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
        else if (payload == SOURCE_ROOT_CONTROL_SEND_RESUME) root_send_resume_turn();
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
        && group.pending_send == NULL);

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
    if (!future || !future->live || !future->actor) return 0;
    SourceActor* actor = future->actor;
    return actor->magic == SOURCE_ACTOR_MAGIC
        && group.vm
        && actor->ref.actor_slot < group.next_slot
        && group.actors[actor->ref.actor_slot] == actor;
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
    SourceActor* actor = future->actor;
    OresNativeVM* vm = group.vm;
    const OresNativeAsyncSendTicket ticket = future->ticket;
    frame.actor = actor;
    ensure(group.root_ref.scheduler_domain
        && group.pending_ready == NULL
        && group.pending_done == NULL
        && group.pending_send == NULL);
    frame.resume_future = ores_native_vm_actor_future_open(
        vm,
        ORES_NATIVE_VM_MAIN,
        group.root_ref,
        SOURCE_ROOT_CONTROL_SEND_RESUME);
    ensure(frame.resume_future != NULL);
    future->consuming = 1;
    group.pending_send = &frame;

    /*
     * Capture the root logical continuation before observing send settlement.
     * Even an already-READY send Future must cross a real suspend/resume pair
     * so post-await guest code never continues on the original host stack.
     */
    ensure(ores_native_vm_publish_control(
        vm,
        ORES_NATIVE_VM_MAIN,
        group.root_ref,
        SOURCE_ROOT_CONTROL_SEND_SUSPEND));
    while (!frame.suspend_recorded)
        ensure(pthread_cond_wait(&frame.completed, &group.mutex) == 0);
    group_unlock();

    OresNativeSendStatus outcome = ORES_NATIVE_SEND_INVALID_REF;
    ensure(ores_native_vm_await_async_send(
        vm, ORES_NATIVE_VM_MAIN, ticket, &outcome));

    group_lock();
    ensure(group.vm == vm && group.pending_send == &frame
        && source_send_future_valid_locked(future)
        && future->actor == actor);
    frame.outcome = (int)outcome;
    ensure(ores_native_actor_future_status(frame.resume_future)
        == ORES_NATIVE_FUTURE_PENDING);
    ensure(ores_native_actor_future_complete(
        frame.resume_future,
        outcome == ORES_NATIVE_SEND_ACCEPTED
            ? ORES_NATIVE_FUTURE_READY
            : ORES_NATIVE_FUTURE_FAILED));

    while (!frame.done)
        ensure(pthread_cond_wait(&frame.completed, &group.mutex) == 0);

    ensure(group.pending_send == &frame);
    group.pending_send = NULL;
    const int64_t result = frame.result;
    ores_native_actor_future_destroy(frame.resume_future);
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
        "SOURCE_AWAIT_SEND_FUTURE_TRACE_PASS outcome=%d execution_id_persisted=1 fresh_resume=1 root_slot=%u source_token=%llu\n",
        (int)outcome,
        SOURCE_GROUP_ROOT_SLOT,
        (unsigned long long)source_token);
#endif
    *result_out = result;
    return (int)outcome;
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
        && group.pending_send == NULL);
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
        && group.pending_send == NULL);
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
            || instance->send_future_live != 0)
        ensure(pthread_cond_wait(
            &group.async_waiters_changed, &group.mutex) == 0);
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
            && group.pending_send == NULL);
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
