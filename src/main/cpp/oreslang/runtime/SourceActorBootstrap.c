#include "oreslang/runtime/SourceActorBootstrap.h"
#include "oreslang/runtime/NativeActorLogicalFramesV1.h"
#include "oreslang/runtime/NativeOresVM.h"
#include "oreslang/runtime/NativeAwaitTraceBridge.h"
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

#include "oreslang/runtime/SourceActorProtocol.h"

typedef struct {
    uint64_t magic;
    uint64_t id;
    OresActorI64Heap* heap;
    OresNativeActorLogicalFramesV1* logical_frames;
    atomic_int logical_frame_failed;
    OresNativeActorRef ref;
    OresSourceActorInit constructor;
    OresSourceActorMessage on_message;
    SourceProtocol* protocol;
    atomic_int initialized;
    atomic_int startup_state;
    atomic_int constructor_started;
    atomic_int stop_requested;
    atomic_int join_started;
    atomic_uint_fast64_t processed;
    atomic_uint_fast64_t successful;
    atomic_uint_fast64_t failed;
    atomic_uint_fast64_t failure_code;
    atomic_int_fast64_t last_result;
} SourceActor;

typedef struct {
    pthread_cond_t completed;
    OresNativeActorFuture* ready_future;
    OresSourceActorContinuation continuation;
    OresSourceActorIdContinuation id_continuation;
    SourceActor* actor;
    OresSourceActorId actor_id;
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
    OresSourceActorIdContinuation id_continuation;
    SourceActor* actor;
    OresSourceActorId actor_id;
    OresNativeSuspensionLink link;
    uint64_t source_token;
    int suspend_recorded;
    int done;
    int64_t result;
} DoneAwaitFrame;

typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t observer_drained;
    OresNativeVM* vm;
    OresNativeActorRef root_ref;
    OresNativeExecutionContextStore* executions;
    OresNativeContinuationStore* continuations;
    ReadyAwaitFrame* pending_ready;
    DoneAwaitFrame* pending_done;
    SourceActor* actors[SOURCE_GROUP_SLOTS];
    size_t next_slot;
    size_t live;
    size_t peak;
    atomic_uint_fast64_t terminal_notices;
    atomic_uint_fast64_t terminal_panic_notices;
    atomic_uint_fast64_t terminal_stopped_notices;
    atomic_uint_fast64_t last_panic_code;
} SourceActorGroup;

/* Process-local trusted-main group; the compiler only supports linear refs
 * in a zero-argument top-level main. There is exactly one worker pool per
 * active group, not one pool per actor.
 */
static SourceActorGroup group = {
    .mutex = PTHREAD_MUTEX_INITIALIZER,
    .observer_drained = PTHREAD_COND_INITIALIZER
};
/* Protected by group.mutex. Never wraps or resets across VM group reopen. */
static uint64_t next_actor_id = UINT64_C(4100);
static _Thread_local OresNativeExecutionHandle current_source_execution;

/* Defined below; the test hooks in the next block call them first. C99 and
 * later reject implicit declarations (an error in clang 16+). */
static void ensure(int condition);
static void group_lock(void);
static void group_unlock(void);

/* Trusted compiler-generated, *synchronous* native actor-frame ABI.
 * The physical worker token is never sent to the guest. Carrier and actor
 * identity are checked by NativeActorLogicalFramesV1 on every operation.
 * This first scalar source profile does not suspend or migrate in a handler.
 */
static _Thread_local SourceActor* frame_actor;
static _Thread_local OresLogicalTurnV1 frame_turn;
static _Thread_local OresLogicalFrameV1 frame_handle;
static _Thread_local int frame_active;
static _Thread_local int frame_faulted;

static int reject_compiled_frame(void) {
    frame_faulted = 1;
    if (frame_actor) {
        atomic_store_explicit(&frame_actor->logical_frame_failed, 1, memory_order_release);
        atomic_store_explicit(&frame_actor->failure_code,
                              ORES_SOURCE_ACTOR_FAILURE_FRAME, memory_order_release);
    }
    return 0;
}

/* One compiled scalar receiver frame (64-byte preallocated slot) per turn.
 * These are not a guest FFI: they are emitted only by LLVM AOT actor handlers.
 * Ref32/ActorRef never carry the private frame store or physical turn token.
 */
int oreslang_source_actor_frame_push(void) {
    if (!frame_actor || frame_faulted || frame_active) return reject_compiled_frame();
    if (!ores_logical_frames_v1_push(frame_actor->logical_frames,
                                    frame_turn, &frame_handle))
        return reject_compiled_frame();
    frame_active = 1;
    return 1;
}
int oreslang_source_actor_frame_store_i64(int64_t value) {
    if (!frame_actor || !frame_active || frame_faulted ||
        !ores_logical_frames_v1_write(frame_actor->logical_frames,
                                     frame_handle, 0, &value, sizeof(value)))
        return reject_compiled_frame();
    return 1;
}
int oreslang_source_actor_frame_load_i64(int64_t* out) {
    if (!frame_actor || !frame_active || frame_faulted || !out ||
        !ores_logical_frames_v1_read(frame_actor->logical_frames,
                                    frame_handle, 0, out, sizeof(*out)))
        return reject_compiled_frame();
    return 1;
}
int oreslang_source_actor_frame_pop(void) {
    if (!frame_actor || !frame_active || frame_faulted ||
        !ores_logical_frames_v1_pop(frame_actor->logical_frames, frame_handle))
        return reject_compiled_frame();
    frame_active = 0;
    frame_handle = (OresLogicalFrameV1){0};
    return 1;
}

#ifdef ORESLANG_SOURCE_ACTOR_TESTING
static atomic_int test_hold_initialization;
static atomic_int test_initialization_entered;
static atomic_int test_hold_readiness_publication;
static atomic_int test_readiness_publication_entered;
/* Deterministic pointer-address reuse is test-only. Normal source execution
 * physically frees every SourceActor allocation after verified DONE. */
static int test_force_address_reuse;
static SourceActor* test_recycled_actor;
static uint64_t test_address_reuses;

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
uint64_t oreslang_source_actor_test_terminal_notices(void) {
    return atomic_load_explicit(&group.terminal_notices, memory_order_acquire);
}
uint64_t oreslang_source_actor_test_panic_notices(void) {
    return atomic_load_explicit(&group.terminal_panic_notices, memory_order_acquire);
}
uint64_t oreslang_source_actor_test_stopped_notices(void) {
    return atomic_load_explicit(&group.terminal_stopped_notices, memory_order_acquire);
}
uint64_t oreslang_source_actor_test_last_panic_code(void) {
    return atomic_load_explicit(&group.last_panic_code, memory_order_acquire);
}

void oreslang_source_actor_test_force_address_reuse(int enabled) {
    group_lock();
    test_force_address_reuse = !!enabled;
    if (!enabled && test_recycled_actor) {
        free(test_recycled_actor);
        test_recycled_actor = NULL;
    }
    group_unlock();
}

uint64_t oreslang_source_actor_test_address_reuses(void) {
    group_lock();
    const uint64_t count = test_address_reuses;
    group_unlock();
    return count;
}

void oreslang_source_actor_test_advance_next_actor_id(uint64_t candidate) {
    group_lock();
    ensure(!group.vm && group.live == 0 &&
        candidate != 0 && candidate >= next_actor_id);
    next_actor_id = candidate;
    group_unlock();
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

/* All checked by-value identity consumers resolve under group.mutex.
 * Never convert an ID into a pointer, or let malloc address reuse confer
 * authority on an expired incarnation. The process-wide counter does not
 * reset when the source VM group closes, so scheduler slot generations and
 * allocator address collisions are irrelevant to this lookup. */
static SourceActor* lookup_actor_id_locked(OresSourceActorId id) {
    if (id == 0 || !group.vm) return NULL;
    for (size_t slot = 0; slot < group.next_slot; ++slot) {
        SourceActor* actor = group.actors[slot];
        if (actor && actor->id == id) {
            ensure(actor->magic == SOURCE_ACTOR_MAGIC
                && actor->ref.actor_slot == slot);
            return actor;
        }
    }
    return NULL;
}


/* Checked await DONE reserves exclusive physical-join authority before
 * suspension. Native pointers remain internal, never guest-visible IDs. */
static SourceActor* live_actor_pointer_locked(void* opaque);
static void claim_actor_join_locked(SourceActor* actor);
static void source_actor_join_claimed(
    SourceActor* instance, OresNativeVM* vm, OresNativeActorRef target);


int oreslang_source_actor_ready_status(void* opaque) {
    /* Compatibility-only: reject a no-longer-registered pointer before
     * dereference. This still cannot distinguish reused malloc addresses. */
    group_lock();
    SourceActor* instance = live_actor_pointer_locked(opaque);
    const int status = instance
        ? atomic_load_explicit(
            &instance->startup_state, memory_order_acquire) : -1;
    group_unlock();
    return status;
}

int oreslang_source_actor_ready_status_id(OresSourceActorId id) {
    group_lock();
    SourceActor* actor = lookup_actor_id_locked(id);
    const int status = actor
        ? atomic_load_explicit(&actor->startup_state, memory_order_acquire)
        : -1;
    group_unlock();
    return status;
}

int oreslang_source_actor_message_receipt(
        void* opaque, OresSourceActorMessageReceipt* out) {
    SourceActor* instance = (SourceActor*)opaque;
    if (!instance || instance->magic != SOURCE_ACTOR_MAGIC || !out) return 0;
    *out = (OresSourceActorMessageReceipt){
        .processed_callbacks = atomic_load_explicit(&instance->processed, memory_order_acquire),
        .successful_messages = atomic_load_explicit(&instance->successful, memory_order_acquire),
        .failed_messages = atomic_load_explicit(&instance->failed, memory_order_acquire),
        .failure_code = atomic_load_explicit(&instance->failure_code, memory_order_acquire),
        .last_success_result = atomic_load_explicit(&instance->last_result, memory_order_acquire),
        .stop_requested = atomic_load_explicit(&instance->stop_requested, memory_order_acquire)
    };
    return 1;
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
    OresSourceActorIdContinuation id_continuation;
    SourceActor* actor;
    OresSourceActorId actor_id;
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
    id_continuation = frame->id_continuation;
    actor = frame->actor;
    actor_id = frame->actor_id;
    ensure((continuation != NULL) != (id_continuation != NULL));
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
        result = id_continuation
            ? id_continuation(actor_id)
            : continuation(actor);
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
    OresSourceActorIdContinuation id_continuation;
    SourceActor* actor;
    OresSourceActorId actor_id;
    OresNativeSuspensionLink link;

    group_lock();
    frame = group.pending_done;
    ensure(frame && frame->suspend_recorded && !frame->done);
    ensure(frame->done_future != NULL);
    ensure(ores_native_actor_future_status(frame->done_future)
        == ORES_NATIVE_FUTURE_READY);
    continuation = frame->continuation;
    id_continuation = frame->id_continuation;
    actor = frame->actor;
    actor_id = frame->actor_id;
    ensure((continuation != NULL) != (id_continuation != NULL));
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
    const int64_t result = id_continuation
        ? id_continuation(actor_id)
        : continuation(actor);
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

static void initialize(void* raw, size_t slot, uint64_t payload) {
    ensure(raw == &group);
    if (slot == SOURCE_GROUP_ROOT_SLOT) {
        if (payload == SOURCE_ROOT_CONTROL_SUSPEND) root_suspend_turn();
        else if (payload == SOURCE_ROOT_CONTROL_RESUME) root_resume_turn();
        else if (payload == SOURCE_ROOT_CONTROL_DONE_SUSPEND) root_done_suspend_turn();
        else if (payload == SOURCE_ROOT_CONTROL_DONE_RESUME) root_done_resume_turn();
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

/* Called only from the VM's serial DATA delivery callback. Each occupied
 * slot has exactly one admitted message. STOP shares the publication gate. */
static void deliver_protocol(SourceActor* actor, uint64_t ticket) {
    group_lock();
    SourceProtocol* protocol = actor->protocol;
    ensure(ticket && ticket <= SOURCE_GROUP_MAILBOX);
    SourceProtocolFrame* frame = &protocol->pending[ticket - 1];
    ensure(frame->occupied && protocol->pending_count);
    ensure(protocol->receipt_count < SOURCE_GROUP_MAILBOX);
    const uint16_t source = protocol->state;
    uint16_t destination = source;
    unsigned status = protocol_transition(protocol, frame, &destination);
    if (status == PROTOCOL_COMMITTED) {
        if (atomic_load_explicit(&actor->stop_requested, memory_order_acquire)) {
            status = PROTOCOL_INTERRUPTED;
            destination = source;
        } else {
            protocol->state = destination;
        }
    }
    frame->occupied = 0;
    --protocol->pending_count;

    const size_t tail = (protocol->receipt_head + protocol->receipt_count) % SOURCE_GROUP_MAILBOX;
    uint8_t* receipt = protocol->receipts[tail];
    memcpy(receipt, "ATPR\1", 5);
    protocol_write_u64(receipt + 5, actor->id);
    protocol_write_u64(receipt + 13, 1);
    protocol_write_u64(receipt + 21, ++protocol->turn);
    receipt[29] = (uint8_t)status;
    protocol_write_u16(receipt + 30, source);
    protocol_write_u16(receipt + 32, destination);
    ++protocol->receipt_count;
    atomic_fetch_add_explicit(&actor->successful, 1, memory_order_release);
    atomic_fetch_add_explicit(&actor->processed, 1, memory_order_release);
    group_unlock();
}

static void deliver(void* raw, size_t slot, uint64_t payload) {
    ensure(raw == &group && slot != SOURCE_GROUP_ROOT_SLOT);
    SourceActor* instance = lookup(slot);
    ensure(atomic_load_explicit(
        &instance->initialized, memory_order_acquire) == 1);
    if (instance->protocol) {
        deliver_protocol(instance, payload);
        return;
    }
    ensure(instance->on_message != NULL);
    OresActorTurn turn =
        ores_actor_i64_heap_enter(instance->heap, instance->id, 1);
    OresLogicalTurnV1 logical = {0};
    int64_t result = 0;
    int succeeded = 0;

    /* This scalar ABI serializes source receiver turns on the scheduler lane.
     * A successful native callback can return zero; zero is never our
     * failure sentinel. Admission and frame drop status decide success. */
    if (!atomic_load_explicit(&instance->logical_frame_failed, memory_order_acquire)
        && ores_logical_frames_v1_begin(instance->logical_frames, instance->id, 1, &logical)) {
        frame_actor = instance;
        frame_turn = logical;
        frame_handle = (OresLogicalFrameV1){0};
        frame_active = 0;
        frame_faulted = 0;

        result = instance->on_message(
            instance->heap, turn.domain, turn.epoch, (int64_t)payload);

        if (!frame_faulted && !frame_active
            && ores_logical_frames_v1_end(instance->logical_frames, logical)) {
            succeeded = 1;
        } else {
            /* Cancel owns scrubbing even when the generated receiver forgot
             * to pop or a frame read/write failed. No output from this
             * callback is treated as a successful guest result. */
            atomic_store_explicit(&instance->logical_frame_failed, 1, memory_order_release);
            atomic_store_explicit(&instance->failure_code,
                                  ORES_SOURCE_ACTOR_FAILURE_FRAME, memory_order_release);
            (void)ores_logical_frames_v1_cancel(instance->logical_frames, logical);
        }
        frame_actor = NULL;
        frame_turn = (OresLogicalTurnV1){0};
        frame_handle = (OresLogicalFrameV1){0};
        frame_active = 0;
        frame_faulted = 0;
    } else {
        atomic_store_explicit(&instance->logical_frame_failed, 1, memory_order_release);
        atomic_store_explicit(&instance->failure_code,
                              ORES_SOURCE_ACTOR_FAILURE_FRAME, memory_order_release);
    }
    ores_actor_i64_heap_leave(instance->heap, turn);

    if (succeeded) {
        atomic_store_explicit(&instance->last_result, result, memory_order_relaxed);
        atomic_fetch_add_explicit(&instance->successful, 1, memory_order_release);
    } else {
        atomic_fetch_add_explicit(&instance->failed, 1, memory_order_release);
    }
    /* Matches the scheduler's *physically processed* DATA callback count.
     * A failed callback is processed by the carrier, not a successful turn. */
    atomic_fetch_add_explicit(&instance->processed, 1, memory_order_release);

    if (!succeeded) {
        /* Seal future DATA immediately on a frame failure while allowing this
         * callback to finish its physical carrier turn. Stop is idempotent
         * against a trusted outer-host request racing this callback. */
        group_lock();
        int expected = 0;
        if (atomic_compare_exchange_strong_explicit(
                &instance->stop_requested, &expected, 1,
                memory_order_acq_rel, memory_order_acquire)) {
            ensure(ores_native_vm_stop_sealed(
                group.vm, ORES_NATIVE_VM_MAIN, instance->ref));
        }
        group_unlock();
    }
}

/* Typed priority CONTROL belongs to the root supervisor's actor lane.
 * Do not look up freed child pointers: only immutable scheduler identities
 * and the typed, runtime-issued terminal envelope cross this boundary.
 * Policy-driven automatic restart is a separate compiler/runtime milestone.
 */
static void source_supervisor_notice(
        void* raw, size_t parent_slot, OresNativeActorTerminalFailure failure) {
    ensure(raw == &group && parent_slot == SOURCE_GROUP_ROOT_SLOT);
    ensure(failure.child.scheduler_domain == group.root_ref.scheduler_domain
        && failure.child.actor_slot < SOURCE_GROUP_ROOT_SLOT
        && failure.child.generation != 0 && failure.correlation != 0);
    if (failure.kind == ORES_NATIVE_CONTROL_PANIC) {
        ensure(failure.failure_code != 0);
        atomic_store_explicit(&group.last_panic_code,
                              failure.failure_code, memory_order_release);
        atomic_fetch_add_explicit(
            &group.terminal_panic_notices, 1, memory_order_release);
    } else {
        ensure(failure.kind == ORES_NATIVE_CONTROL_STOPPED
            && failure.failure_code == 0);
        atomic_fetch_add_explicit(
            &group.terminal_stopped_notices, 1, memory_order_release);
    }
    atomic_fetch_add_explicit(&group.terminal_notices, 1, memory_order_release);
}

static void open_group_locked(void) {
    ensure(!group.vm && !group.executions && !group.continuations);
    ensure(group.live == 0 && group.next_slot == 0 && group.peak == 0);
    ensure(group.pending_ready == NULL && group.pending_done == NULL);
    atomic_store_explicit(&group.terminal_notices, 0, memory_order_release);
    atomic_store_explicit(&group.terminal_panic_notices, 0, memory_order_release);
    atomic_store_explicit(&group.terminal_stopped_notices, 0, memory_order_release);
    atomic_store_explicit(&group.last_panic_code, 0, memory_order_release);

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
        cfg.domains[i].on_failure = source_supervisor_notice;
        cfg.domains[i].context = &group;
    }
    group.vm = ores_native_vm_open(&cfg);
    ensure(group.vm != NULL);
    /* Match the Java VM CONTROL bootstrap barrier: one native master loop is
     * running before the root actor can admit work. It only signals scheduler
     * conditions; constructor/guest execution stays on bounded carriers. */
    ensure(ores_native_vm_start_main_control_loop(group.vm));

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

static SourceActor* spawn_source_actor(
        OresSourceActorInit constructor,
        OresSourceActorMessage on_message,
        uint64_t fields,
        SourceProtocol* protocol) {
    ensure(constructor != NULL && fields >= 1 && fields <= 4096);
    group_lock();
    if (!group.vm) open_group_locked();

    ensure(group.next_slot < SOURCE_GROUP_ROOT_SLOT);
    const size_t slot = group.next_slot++;
    SourceActor* instance = NULL;
#ifdef ORESLANG_SOURCE_ACTOR_TESTING
    if (test_force_address_reuse && test_recycled_actor) {
        instance = test_recycled_actor;
        test_recycled_actor = NULL;
        memset(instance, 0, sizeof(*instance));
        ++test_address_reuses;
    }
#endif
    if (!instance) instance = (SourceActor*)calloc(1, sizeof(*instance));
    ensure(instance != NULL);
    instance->magic = SOURCE_ACTOR_MAGIC;
    /* Fail closed on exhaustion: never wrap/reissue an old actor ID.
     * Atomics are unnecessary because only group.mutex admits spawn. */
#ifdef ORESLANG_SOURCE_ACTOR_TESTING
    if (next_actor_id == 0 || next_actor_id == UINT64_MAX)
        fputs("SOURCE_ACTOR_CHECKED_ID_EXHAUSTED\n", stderr);
#endif
    ensure(next_actor_id != 0 && next_actor_id < UINT64_MAX);
    instance->id = next_actor_id++;
    instance->constructor = constructor;
    instance->on_message = on_message;
    instance->protocol = protocol;

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
    atomic_init(&instance->successful, 0);
    atomic_init(&instance->failed, 0);
    atomic_init(&instance->failure_code, 0);
    atomic_init(&instance->last_result, 0);
    atomic_init(&instance->logical_frame_failed, 0);

    instance->heap = ores_actor_i64_heap_open(instance->id, 1, fields);
    /* Current native i64 heap is serial. One preallocated turn lane, with
     * reserved depth for future compiler-generated nested synchronous calls. */
    instance->logical_frames = ores_logical_frames_v1_open(instance->id, 1, 1, 16, 64);
    ensure(instance->logical_frames != NULL);
    group.actors[slot] = instance;
    /* Every compiler-generated source actor has a registered root supervisor
     * (host lifecycle metadata; no guest supervisor pointer). Unlike the
     * metadata-only compiler root, a source actor becomes externally
     * observable after this call, so require a live native worker (with its
     * run-loop check-in) before publishing the supervised actor handle. */
    instance->ref = ores_native_vm_spawn_supervised_ready(
        group.vm, ORES_NATIVE_VM_MAIN, slot, group.root_ref);
    ensure(instance->ref.scheduler_domain
        && instance->ref.actor_slot == slot);
#ifdef ORESLANG_SOURCE_ACTOR_TESTING
    if (slot == 0) {
        /* Exact-source e2e receipt: the actor is not published with a
         * phantom carrier, and no other domain starts from an idle spawn. */
        ensure(ores_native_vm_started_carriers(
            group.vm, ORES_NATIVE_VM_MAIN) == 1);
        ensure(ores_native_vm_started_carriers(
            group.vm, ORES_NATIVE_VM_SHARED) == 0);
        ensure(ores_native_vm_started_carriers(
            group.vm, ORES_NATIVE_VM_TRUSTED_PRIVATE) == 0);
        ensure(ores_native_vm_started_carriers(
            group.vm, ORES_NATIVE_VM_UNTRUSTED) == 0);
        fprintf(stderr,
            "SOURCE_ACTOR_FIRST_CARRIER_READY main=1 shared=0 private=0 untrusted=0\\n");
    }
#endif
    group.live++;
    if (group.live > group.peak) group.peak = group.live;

    /* CONTROL initializes the compiler-emitted actor constructor on a worker. */
    ensure(ores_native_vm_publish_control(
        group.vm, ORES_NATIVE_VM_MAIN, instance->ref, 0));
    group_unlock();
    return instance;
}

void* oreslang_source_actor_spawn(
        OresSourceActorInit constructor, OresSourceActorMessage on_message, uint64_t fields) {
    return spawn_source_actor(constructor, on_message, fields, NULL);
}

static void init_protocol(OresActorI64Heap* heap, uint64_t domain, uint64_t epoch) {
    (void)heap;
    (void)domain;
    (void)epoch;
}

OresSourceActorId oreslang_source_actor_spawn_protocol_id(
        const uint8_t* plan, uint64_t length) {
    if (length > SIZE_MAX) return 0;
    SourceProtocol* protocol = protocol_open(plan, (size_t)length);
    return protocol ? spawn_source_actor(init_protocol, NULL, 1, protocol)->id : 0;
}

uint64_t oreslang_source_actor_encode_protocol_data(const uint8_t* digest,
        uint64_t tag, const int64_t* fields, uint64_t count,
        uint8_t* out, uint64_t capacity) {
    if (!digest || tag > UINT16_MAX || count > PROTOCOL_MAX_FIELDS ||
        (count && !fields) || !out || capacity < PROTOCOL_HEADER_SIZE + 8 * count)
        return 0;
    memcpy(out, "ATPM\1", 5);
    memcpy(out + 5, digest, PROTOCOL_DIGEST_SIZE);
    protocol_write_u16(out + 37, (uint16_t)tag);
    for (size_t i = 0; i < count; ++i)
        protocol_write_u64(out + PROTOCOL_HEADER_SIZE + 8 * i, (uint64_t)fields[i]);
    return PROTOCOL_HEADER_SIZE + 8 * count;
}

int oreslang_source_actor_try_send_protocol_id(OresSourceActorId id,
        const uint8_t* bytes, uint64_t length) {
    if (!bytes && length) return 0;
    group_lock();
    SourceActor* actor = lookup_actor_id_locked(id);
    SourceProtocol* protocol = actor ? actor->protocol : NULL;
    int accepted = 0;
    if (protocol && !atomic_load_explicit(&actor->stop_requested, memory_order_acquire) &&
        protocol->pending_count + protocol->receipt_count < SOURCE_GROUP_MAILBOX &&
        protocol->turn < UINT64_MAX - protocol->pending_count) {
        size_t slot = 0;
        while (slot < SOURCE_GROUP_MAILBOX && protocol->pending[slot].occupied) ++slot;
        ensure(slot < SOURCE_GROUP_MAILBOX);
        SourceProtocolFrame* frame = &protocol->pending[slot];
        /* Copy only the maximum canonical frame. Preserve oversize as a
         * length mismatch without allocating storage proportional to input. */
        const size_t copy = length > sizeof(frame->bytes) ? sizeof(frame->bytes) : (size_t)length;
        if (copy) memcpy(frame->bytes, bytes, copy);
        frame->occupied = 1;
        frame->length = length > copy ? copy + 1 : copy;
        accepted = ores_native_vm_try_send(group.vm, ORES_NATIVE_VM_MAIN, actor->ref, slot + 1)
            == ORES_NATIVE_SEND_ACCEPTED;
        if (accepted) ++protocol->pending_count;
        else frame->occupied = 0;
    }
    group_unlock();
    return accepted;
}

int oreslang_source_actor_try_pop_protocol_receipt_id(
        OresSourceActorId id, uint8_t* out, uint64_t capacity) {
    if (!out || capacity < ORES_SOURCE_PROTOCOL_RECEIPT_SIZE) return 0;
    group_lock();
    SourceActor* actor = lookup_actor_id_locked(id);
    SourceProtocol* protocol = actor ? actor->protocol : NULL;
    const int available = protocol && protocol->receipt_count;
    if (available) {
        memcpy(out, protocol->receipts[protocol->receipt_head], ORES_SOURCE_PROTOCOL_RECEIPT_SIZE);
        protocol->receipt_head = (protocol->receipt_head + 1) % SOURCE_GROUP_MAILBOX;
        --protocol->receipt_count;
    }
    group_unlock();
    return available;
}

/* Checked identity is a minted value, not an encoded host pointer. The
 * returning caller cannot race with another actor's stop before it knows
 * the new identity, and group.mutex validates its live allocation. */
OresSourceActorId oreslang_source_actor_spawn_id(
        OresSourceActorInit constructor,
        OresSourceActorMessage on_message,
        uint64_t fields) {
    SourceActor* actor = (SourceActor*)oreslang_source_actor_spawn(
        constructor, on_message, fields);
    group_lock();
    ensure(actor && actor->magic == SOURCE_ACTOR_MAGIC);
    const OresSourceActorId identity = actor->id;
    ensure(lookup_actor_id_locked(identity) == actor);
    group_unlock();
    return identity;
}

int oreslang_source_actor_try_send_id(
        OresSourceActorId identity, int64_t payload) {
    group_lock();
    SourceActor* actor = lookup_actor_id_locked(identity);
    const int accepted = actor && actor->on_message &&
        ores_native_vm_try_send(
            group.vm, ORES_NATIVE_VM_MAIN, actor->ref, (uint64_t)payload)
            == ORES_NATIVE_SEND_ACCEPTED;
    group_unlock();
    return accepted;
}

void oreslang_source_actor_send_id(
        OresSourceActorId identity, int64_t payload) {
    const int accepted = oreslang_source_actor_try_send_id(identity, payload);
#ifdef ORESLANG_SOURCE_ACTOR_TESTING
    if (!accepted)
        fputs("SOURCE_ACTOR_CHECKED_ID_REJECTED\n", stderr);
#endif
    ensure(accepted);
}

int oreslang_source_actor_try_send(void* opaque, int64_t payload) {
    /* Serialize pointer membership and nonblocking mailbox admission with
     * join/free; never read an expired allocation before taking group.mutex. */
    group_lock();
    SourceActor* instance = live_actor_pointer_locked(opaque);
#ifdef ORESLANG_SOURCE_ACTOR_TESTING
    if (!instance)
        fputs("SOURCE_ACTOR_LEGACY_STALE_POINTER_DENIED\n", stderr);
#endif
    const int accepted = instance && instance->magic == SOURCE_ACTOR_MAGIC
        && instance->on_message
        && !atomic_load_explicit(&instance->logical_frame_failed, memory_order_acquire)
        && !atomic_load_explicit(&instance->stop_requested, memory_order_acquire)
        && ores_native_vm_try_send(
            group.vm, ORES_NATIVE_VM_MAIN, instance->ref, (uint64_t)payload)
            == ORES_NATIVE_SEND_ACCEPTED;
    group_unlock();
    return accepted;
}

void oreslang_source_actor_send(void* opaque, int64_t payload) {
    ensure(oreslang_source_actor_try_send(opaque, payload));
}

static int source_actor_await_ready_common(
        void* opaque,
        OresSourceActorId identity,
        OresSourceActorContinuation continuation,
        OresSourceActorIdContinuation id_continuation,
        uint64_t source_token,
        int64_t* result_out) {
    ensure((continuation != NULL) != (id_continuation != NULL));
    ensure(result_out != NULL);

    ReadyAwaitFrame frame = {
        .continuation = continuation,
        .id_continuation = id_continuation,
        .actor_id = identity,
        .source_token = source_token
    };
    ensure(pthread_cond_init(&frame.completed, NULL) == 0);
    group_lock();
    SourceActor* instance = id_continuation
        ? lookup_actor_id_locked(identity)
        : live_actor_pointer_locked(opaque);
#ifdef ORESLANG_SOURCE_ACTOR_TESTING
    if (id_continuation && !instance)
        fputs("SOURCE_ACTOR_CHECKED_ID_REJECTED\n", stderr);
#endif
    ensure(instance && instance->magic == SOURCE_ACTOR_MAGIC);
    frame.actor = instance;
    ensure(group.vm && group.root_ref.scheduler_domain
        && group.pending_ready == NULL && group.pending_done == NULL);
    /* Blocking occurs on this trusted outer host thread only. No carrier
     * from ANY VM domain may be parked while waiting for the root CONTROL
     * continuation to resume and drain its logical execution context. */
    OresNativeExecutionContext carrier = {0};
    for (int domain = 0; domain < ORES_NATIVE_VM_DOMAIN_COUNT; ++domain)
        ensure(!ores_native_vm_current_execution_context(
            group.vm, (enum OresNativeVMDomain)domain, &carrier));
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
    ensure(pthread_cond_broadcast(&group.observer_drained) == 0);
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

int oreslang_source_actor_await_ready_checked(
        void* opaque,
        OresSourceActorContinuation continuation,
        uint64_t source_token,
        int64_t* result_out) {
    return source_actor_await_ready_common(
        opaque, 0, continuation, NULL, source_token, result_out);
}

int oreslang_source_actor_await_ready_id_checked(
        OresSourceActorId identity,
        OresSourceActorIdContinuation continuation,
        uint64_t source_token,
        int64_t* result_out) {
    return source_actor_await_ready_common(
        NULL, identity, NULL, continuation, source_token, result_out);
}

int64_t oreslang_source_actor_await_ready_id(
        OresSourceActorId identity,
        OresSourceActorIdContinuation continuation,
        uint64_t source_token) {
    int64_t result = 0;
    const int outcome = oreslang_source_actor_await_ready_id_checked(
        identity, continuation, source_token, &result);
    /* The typed ActorTerminated guest projection is a separate language
     * addition. A failed READY must not execute the source continuation. */
    ensure(outcome == ORES_SOURCE_ACTOR_STARTUP_READY);
    return result;
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

static int64_t source_actor_await_done_common(
        void* opaque,
        OresSourceActorId identity,
        OresSourceActorContinuation continuation,
        OresSourceActorIdContinuation id_continuation,
        uint64_t source_token) {
    ensure((continuation != NULL) != (id_continuation != NULL));
    DoneAwaitFrame frame = {
        .continuation = continuation,
        .id_continuation = id_continuation,
        .actor_id = identity,
        .source_token = source_token
    };
    ensure(pthread_cond_init(&frame.completed, NULL) == 0);

    group_lock();
    SourceActor* instance = id_continuation
        ? lookup_actor_id_locked(identity)
        : live_actor_pointer_locked(opaque);
#ifdef ORESLANG_SOURCE_ACTOR_TESTING
    if (id_continuation && !instance)
        fputs("SOURCE_ACTOR_CHECKED_ID_REJECTED\n", stderr);
#endif
    ensure(instance && instance->magic == SOURCE_ACTOR_MAGIC);
    ensure(atomic_load_explicit(
        &instance->stop_requested, memory_order_acquire) == 1);
    ensure(atomic_load_explicit(
        &instance->join_started, memory_order_acquire) == 0);
    ensure(group.vm && group.root_ref.scheduler_domain
        && group.pending_ready == NULL && group.pending_done == NULL);
    /* Claim physical teardown exclusively before any unlocked suspension.
     * This prevents concurrent join from freeing the target while this
     * Future holds a logical continuation/physical target reference. */
    if (id_continuation)
        claim_actor_join_locked(instance);
    frame.actor = instance;
    OresNativeVM* vm = group.vm;
    const OresNativeActorRef target = instance->ref;
    OresNativeExecutionContext carrier = {0};
    for (int domain = 0; domain < ORES_NATIVE_VM_DOMAIN_COUNT; ++domain)
        ensure(!ores_native_vm_current_execution_context(
            vm, (enum OresNativeVMDomain)domain, &carrier));
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
    ensure(pthread_cond_broadcast(&group.observer_drained) == 0);
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
    if (id_continuation)
        source_actor_join_claimed(instance, vm, target);
    else
        oreslang_source_actor_join_done(instance);
    return result;
}


int64_t oreslang_source_actor_await_done(
        void* opaque,
        OresSourceActorContinuation continuation,
        uint64_t source_token) {
    return source_actor_await_done_common(
        opaque, 0, continuation, NULL, source_token);
}

int64_t oreslang_source_actor_await_done_id(
        OresSourceActorId identity,
        OresSourceActorIdContinuation continuation,
        uint64_t source_token) {
    return source_actor_await_done_common(
        NULL, identity, NULL, continuation, source_token);
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

int oreslang_source_actor_try_request_stop_id(OresSourceActorId identity) {
    group_lock();
    SourceActor* actor = lookup_actor_id_locked(identity);
    int accepted = 0;
    if (actor && !atomic_load_explicit(
            &actor->stop_requested, memory_order_acquire)) {
        /* Constructor/READY and STOP order under this SAME group mutex.
         * If the VM declines the stop, do not poison stop_requested. */
        accepted = ores_native_vm_stop_sealed(
            group.vm, ORES_NATIVE_VM_MAIN, actor->ref);
        if (accepted)
            atomic_store_explicit(
                &actor->stop_requested, 1, memory_order_release);
    }
    group_unlock();
    return accepted;
}

void oreslang_source_actor_request_stop_id(OresSourceActorId identity) {
    const int accepted = oreslang_source_actor_try_request_stop_id(identity);
#ifdef ORESLANG_SOURCE_ACTOR_TESTING
    if (!accepted)
        fputs("SOURCE_ACTOR_CHECKED_ID_REJECTED\n", stderr);
#endif
    ensure(accepted);
}

void oreslang_source_actor_request_stop(void* opaque) {
    /*
     * Validate live membership BEFORE dereferencing actor fields. Old pointer
     * values cannot be authenticated across allocator reuse; this is only a
     * UAF mitigation for a completely retired, unregistered allocation.
     */
    group_lock();
    SourceActor* instance = live_actor_pointer_locked(opaque);
#ifdef ORESLANG_SOURCE_ACTOR_TESTING
    if (!instance)
        fputs("SOURCE_ACTOR_LEGACY_STALE_POINTER_DENIED\n", stderr);
#endif
    ensure(instance && instance->magic == SOURCE_ACTOR_MAGIC);
    int expected = 0;
    const int first_request = atomic_compare_exchange_strong_explicit(
        &instance->stop_requested,
        &expected,
        1,
        memory_order_acq_rel,
        memory_order_acquire);
    /* A frame error may already have sealed the mailbox on the carrier.
     * An outer host may still issue its ordinary stop/get_done sequence. */
    ensure(first_request ||
        atomic_load_explicit(&instance->logical_frame_failed, memory_order_acquire));

    const size_t slot = (size_t)instance->ref.actor_slot;
    ensure(group.vm && slot < group.next_slot
        && group.actors[slot] == instance);
    /*
     * Source stop matches Java ActorRef.stop(), not gracefulStop(): reject
     * future DATA and abandon queued DATA, while an already-active turn is
     * allowed to cross its physical execution boundary.
     */
    if (first_request)
        ensure(ores_native_vm_stop_sealed(
            group.vm, ORES_NATIVE_VM_MAIN, instance->ref));
    group_unlock();
}

/* Called ONLY after join_started was exclusively claimed under group
 * mutex. The claimant pins the allocation and VM until this function
 * returns; no other join may free it concurrently. */
static void source_actor_join_claimed(
        SourceActor* instance, OresNativeVM* vm, OresNativeActorRef target) {
    OresNativeExecutionContext carrier = {0};
    for (int domain = 0; domain < ORES_NATIVE_VM_DOMAIN_COUNT; ++domain)
        ensure(!ores_native_vm_current_execution_context(
            vm, (enum OresNativeVMDomain)domain, &carrier));
    const size_t slot = (size_t)target.actor_slot;
    ensure(ores_native_vm_wait_done(
        vm, ORES_NATIVE_VM_MAIN, target));

    group_lock();
    ensure(group.vm == vm && slot < group.next_slot
        && group.actors[slot] == instance && instance->ref.actor_slot == slot);
    /* READY is an admitted logical observer on the root CONTROL lane.
     * A physical DONE acknowledgement is not permission to free memory
     * while that observer may still resume and inspect this actor. */
    while ((group.pending_ready && group.pending_ready->actor == instance) ||
           (group.pending_done && group.pending_done->actor == instance))
        ensure(pthread_cond_wait(&group.observer_drained, &group.mutex) == 0);
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
    /* Actor-origin failures still count as physically returned DATA callbacks
     * at the scheduler layer, but NEVER as successful guest messages. */
    const uint64_t successful = atomic_load_explicit(
        &instance->successful, memory_order_acquire);
    const uint64_t failed = atomic_load_explicit(
        &instance->failed, memory_order_acquire);
    const uint64_t failure_code = atomic_load_explicit(
        &instance->failure_code, memory_order_acquire);
    ensure(successful <= terminal.processed_user_messages
        && failed == terminal.processed_user_messages - successful);
    ensure(failure_code == 0 || failed > 0);
    ensure(terminal.abandoned_control_messages == 0);

    /*
     * get_done parity requires physical quiescence, not a logical stop bit.
     * Reclaim the actor-owned heap only after the VM terminal acknowledgement.
     */
    ores_actor_i64_heap_retire(instance->heap);
    ensure(ores_actor_i64_heap_try_destroy(instance->heap));
    /* Physical STOP acknowledgement gates both heap and logical-frame free. */
    ensure(ores_logical_frames_v1_close(instance->logical_frames));

    /* The VM requires a trusted resource-finalization receipt *after* the
     * physical actor DONE and both actor-owned allocations are gone. Only
     * the registered parent can consume this typed failure/STOPPED notice.
     * Once acknowledged, a duplicate is rejected by the VM. */
    ensure(ores_native_vm_confirm_supervised_finalized(
        group.vm, ORES_NATIVE_VM_MAIN, instance->ref));
    OresNativeActorTerminalFailure notice = {
        .kind = failure_code ? ORES_NATIVE_CONTROL_PANIC : ORES_NATIVE_CONTROL_STOPPED,
        .child = instance->ref,
        .failure_code = failure_code,
        .correlation = instance->id
    };
    /* The reserved parent CONTROL slot is bounded. Publication can fail
     * transiently if occupied; do not lose the notice. We are the external
     * host, never a carrier turn. Release group mutex while waiting, because
     * root CONTROL callbacks may need it to finish. No per-retry allocation. */
    group_unlock();
    while (!ores_native_vm_publish_supervised_failure(
            group.vm, ORES_NATIVE_VM_MAIN, notice))
        sched_yield();
    group_lock();
    ensure(group.vm && slot < group.next_slot
        && group.actors[slot] == instance);
#ifdef ORESLANG_SOURCE_ACTOR_TESTING
    fprintf(stderr,
        "SOURCE_ACTOR_SPAWN_STOP_VERIFIED constructor_on_worker=%d released=1 ready=%d accepted=%llu processed=%llu abandoned=%llu last=%lld successful=%llu failed=%llu failure_code=%llu\n",
        atomic_load_explicit(
            &instance->constructor_started, memory_order_acquire),
        startup_state == ORES_SOURCE_ACTOR_STARTUP_READY,
        (unsigned long long)terminal.accepted_user_messages,
        (unsigned long long)terminal.processed_user_messages,
        (unsigned long long)terminal.abandoned_user_messages,
        (long long)atomic_load_explicit(
            &instance->last_result, memory_order_relaxed),
        (unsigned long long)successful,
        (unsigned long long)failed,
        (unsigned long long)failure_code);
#endif
    group.actors[slot] = NULL;
    free(instance->protocol);
    instance->magic = 0;
#ifdef ORESLANG_SOURCE_ACTOR_TESTING
    if (test_force_address_reuse) {
        /* Test-only single-entry allocator freelist. The next actor gets
         * the SAME SourceActor address; its globally unique ID differs.
         * A bounded single cache avoids indefinite tombstone allocation. */
        ensure(test_recycled_actor == NULL);
        memset(instance, 0, sizeof(*instance));
        test_recycled_actor = instance;
    } else
#endif
    free(instance);
    ensure(group.live > 0);
    group.live--;

    if (group.live == 0) {
        ensure(group.pending_ready == NULL && group.pending_done == NULL);
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


static SourceActor* live_actor_pointer_locked(void* opaque) {
    if (!opaque || !group.vm) return NULL;
    for (size_t slot = 0; slot < group.next_slot; ++slot)
        if (group.actors[slot] == opaque)
            return group.actors[slot];
    return NULL;
}

static void claim_actor_join_locked(SourceActor* actor) {
    ensure(actor != NULL && actor->magic == SOURCE_ACTOR_MAGIC);
    ensure(atomic_load_explicit(
        &actor->stop_requested, memory_order_acquire) == 1);
    int expected = 0;
    ensure(atomic_compare_exchange_strong_explicit(
        &actor->join_started, &expected, 1,
        memory_order_acq_rel, memory_order_acquire));
}

void oreslang_source_actor_join_done_id(OresSourceActorId identity) {
    group_lock();
    SourceActor* actor = lookup_actor_id_locked(identity);
#ifdef ORESLANG_SOURCE_ACTOR_TESTING
    if (!actor)
        fputs("SOURCE_ACTOR_CHECKED_ID_REJECTED\n", stderr);
#endif
    claim_actor_join_locked(actor);
    OresNativeVM* vm = group.vm;
    const OresNativeActorRef target = actor->ref;
    group_unlock();
    source_actor_join_claimed(actor, vm, target);
}

void oreslang_source_actor_stop_id(OresSourceActorId identity) {
    oreslang_source_actor_request_stop_id(identity);
    oreslang_source_actor_join_done_id(identity);
}

/* Compatibility ABI: pointer membership avoids some stale dereferences,
 * but recycled malloc addresses are still ambiguous. Compiler-generated
 * new source must use the identity API, not this legacy path. */
void oreslang_source_actor_join_done(void* opaque) {
    group_lock();
    SourceActor* actor = live_actor_pointer_locked(opaque);
#ifdef ORESLANG_SOURCE_ACTOR_TESTING
    if (!actor)
        fputs("SOURCE_ACTOR_LEGACY_STALE_POINTER_DENIED\n", stderr);
#endif
    claim_actor_join_locked(actor);
    OresNativeVM* vm = group.vm;
    const OresNativeActorRef target = actor->ref;
    group_unlock();
    source_actor_join_claimed(actor, vm, target);
}

void oreslang_source_actor_stop(void* opaque) {
    /*
     * Preserve synchronous stop for source paths that do not await get_done.
     * get_done lowering instead emits request_stop followed by await_done.
     */
    oreslang_source_actor_request_stop(opaque);
    oreslang_source_actor_join_done(opaque);
}
