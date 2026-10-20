#include "oreslang/runtime/SourceActorBootstrap.h"
#include "oreslang/runtime/NativeOresVM.h"
#include "oreslang/runtime/NativeAwaitTraceBridge.h"
#include <pthread.h>
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

typedef struct {
    uint64_t magic;
    uint64_t id;
    OresActorI64Heap* heap;
    OresNativeActorRef ref;
    OresSourceActorInit constructor;
    OresSourceActorMessage on_message;
    atomic_int initialized;
    atomic_uint_fast64_t processed;
    atomic_int_fast64_t last_result;
} SourceActor;

typedef struct {
    pthread_cond_t completed;
    OresNativeActorFuture* ready_future;
    OresSourceActorContinuation continuation;
    SourceActor* actor;
    OresNativeSuspensionLink link;
    uint64_t source_token;
    int suspend_recorded;
    int done;
    int64_t result;
} ReadyAwaitFrame;

typedef struct {
    pthread_mutex_t mutex;
    OresNativeVM* vm;
    OresNativeActorRef root_ref;
    OresNativeExecutionContextStore* executions;
    OresNativeContinuationStore* continuations;
    ReadyAwaitFrame* pending_ready;
    SourceActor* actors[SOURCE_GROUP_SLOTS];
    size_t next_slot;
    size_t live;
    size_t peak;
} SourceActorGroup;

/* Process-local trusted-main group; the compiler only supports linear refs
 * in a zero-argument top-level main. There is exactly one worker pool per
 * active group, not one pool per actor.
 */
static SourceActorGroup group = { .mutex = PTHREAD_MUTEX_INITIALIZER };
static _Atomic uint64_t next_actor_id = 4100;
static _Thread_local OresNativeExecutionHandle current_source_execution;

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

static void complete_ready_if_admitted_locked(ReadyAwaitFrame* frame) {
    ensure(frame && frame == group.pending_ready);
    if (!frame->suspend_recorded
            || atomic_load_explicit(
                &frame->actor->initialized, memory_order_acquire) != 1)
        return;
    ensure(frame->ready_future != NULL);
    if (ores_native_actor_future_status(frame->ready_future)
            == ORES_NATIVE_FUTURE_PENDING) {
        ensure(ores_native_actor_future_complete(
            frame->ready_future, ORES_NATIVE_FUTURE_READY));
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
    complete_ready_if_admitted_locked(frame);
    group_unlock();
}

static void root_resume_turn(void) {
    ReadyAwaitFrame* frame;
    OresSourceActorContinuation continuation;
    SourceActor* actor;
    OresNativeSuspensionLink link;

    group_lock();
    frame = group.pending_ready;
    ensure(frame && frame->suspend_recorded && !frame->done);
    ensure(frame->ready_future != NULL);
    ensure(ores_native_actor_future_status(frame->ready_future)
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

    /*
     * The logical resumed context must describe the physical root actor turn,
     * not the child whose readiness happened to complete the Future.
     */
    ensure(same_allocation(logical.allocation, physical.allocation));
    ensure(!ores_native_store_handle_valid(current_source_execution));
    current_source_execution = resumed;
    const int64_t result = continuation(actor);
    current_source_execution = (OresNativeExecutionHandle){0, 0};

    ensure(ores_native_await_trace_resume_finish(group.continuations, link));
    ensure(ores_native_execution_context_store_release(
        group.executions, resumed));

    group_lock();
    ensure(frame == group.pending_ready && !frame->done);
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
        else abort();
        return;
    }

    ensure(payload == 0);
    SourceActor* instance = lookup(slot);
    OresActorTurn turn =
        ores_actor_i64_heap_enter(instance->heap, instance->id, 1);
    instance->constructor(instance->heap, turn.domain, turn.epoch);
    ores_actor_i64_heap_leave(instance->heap, turn);
    atomic_store_explicit(&instance->initialized, 1, memory_order_release);

    /*
     * Registration may happen before or after initialization. The group mutex
     * makes completion one-shot without holding a scheduler lock across guest
     * construction. source_actor_stop never waits while holding this mutex.
     */
    group_lock();
    ReadyAwaitFrame* frame = group.pending_ready;
    if (frame && frame->actor == instance)
        complete_ready_if_admitted_locked(frame);
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
    ensure(group.pending_ready == NULL);

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

void oreslang_source_actor_send(void* opaque, int64_t payload) {
    SourceActor* instance = (SourceActor*)opaque;
    ensure(instance && instance->magic == SOURCE_ACTOR_MAGIC
        && instance->on_message);
    ensure(ores_native_vm_try_send(
        group.vm, ORES_NATIVE_VM_MAIN, instance->ref, (uint64_t)payload)
        == ORES_NATIVE_SEND_ACCEPTED);
}

int64_t oreslang_source_actor_await_ready(
        void* opaque,
        OresSourceActorContinuation continuation,
        uint64_t source_token) {
    SourceActor* instance = (SourceActor*)opaque;
    ensure(instance && instance->magic == SOURCE_ACTOR_MAGIC);
    ensure(continuation != NULL);

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
        && group.pending_ready == NULL);
    frame.ready_future = ores_native_vm_actor_future_open(
        group.vm,
        ORES_NATIVE_VM_MAIN,
        group.root_ref,
        SOURCE_ROOT_CONTROL_RESUME);
    ensure(frame.ready_future != NULL);
    group.pending_ready = &frame;

    /*
     * First root turn captures/suspends its scheduler-minted ExecutionId.
     * Future completion cannot resume until this CONTROL turn has published
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
    const int64_t result = frame.result;

    ores_native_actor_future_destroy(frame.ready_future);
    ensure(ores_native_continuation_store_release(
        group.continuations, frame.link.continuation));
    ensure(ores_native_execution_context_store_live(group.executions) == 0);
    ensure(ores_native_continuation_store_live(group.continuations) == 0);
    group_unlock();

    ensure(pthread_cond_destroy(&frame.completed) == 0);
#ifdef ORESLANG_SOURCE_ACTOR_TESTING
    fprintf(stderr,
        "SOURCE_AWAIT_READY_TRACE_PASS execution_id_persisted=1 fresh_resume=1 root_slot=%u source_token=%llu\n",
        SOURCE_GROUP_ROOT_SLOT,
        (unsigned long long)source_token);
#endif
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

int oreslang_source_actor_capture_failure_trace(
        OresNativeFailureTraceKind kind,
        uint32_t code,
        OresNativeExecutionTraceFrame* execution_frames,
        size_t execution_capacity,
        OresNativeContinuationTraceFrame* continuation_frames,
        size_t continuation_capacity,
        OresNativeFailureTraceReport* out) {
    if (!out || !ores_native_store_handle_valid(current_source_execution)
            || !group.executions || !group.continuations)
        return 0;
    return ores_native_failure_trace_capture(
        group.executions,
        group.continuations,
        current_source_execution,
        kind,
        code,
        execution_frames,
        execution_capacity,
        continuation_frames,
        continuation_capacity,
        out);
}

void oreslang_source_actor_stop(void* opaque) {
    SourceActor* instance = (SourceActor*)opaque;
    ensure(instance && instance->magic == SOURCE_ACTOR_MAGIC);

    /*
     * Stop admission is serialized with group metadata, but the blocking host
     * wait MUST NOT hold group.mutex: constructor completion may need that
     * mutex to settle a registered readiness Future.
     */
    group_lock();
    const size_t slot = (size_t)instance->ref.actor_slot;
    ensure(group.vm && slot < group.next_slot
        && group.actors[slot] == instance);
    ensure(ores_native_vm_stop(
        group.vm, ORES_NATIVE_VM_MAIN, instance->ref));
    group_unlock();

    ensure(ores_native_vm_wait_done(
        group.vm, ORES_NATIVE_VM_MAIN, instance->ref));

    group_lock();
    ensure(group.actors[slot] == instance);
    ensure(atomic_load_explicit(
        &instance->initialized, memory_order_acquire) == 1);
    ensure(ores_native_vm_poll_done(
        group.vm, ORES_NATIVE_VM_MAIN, instance->ref));

    ores_actor_i64_heap_retire(instance->heap);
    ensure(ores_actor_i64_heap_try_destroy(instance->heap));
#ifdef ORESLANG_SOURCE_ACTOR_TESTING
    fprintf(stderr,
        "SOURCE_ACTOR_SPAWN_STOP_VERIFIED constructor_on_worker=1 released=1 processed=%llu last=%lld\n",
        (unsigned long long)atomic_load_explicit(
            &instance->processed, memory_order_acquire),
        (long long)atomic_load_explicit(
            &instance->last_result, memory_order_relaxed));
#endif
    group.actors[slot] = NULL;
    instance->magic = 0;
    free(instance);
    ensure(group.live > 0);
    group.live--;

    if (group.live == 0) {
        ensure(group.pending_ready == NULL);
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
