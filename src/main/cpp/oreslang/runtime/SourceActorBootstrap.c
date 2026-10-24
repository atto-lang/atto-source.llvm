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
#define SOURCE_ACTOR_OUTPUT_CAPACITY 16u
#define SOURCE_TRACE_EXECUTIONS 128u
#define SOURCE_TRACE_CONTINUATIONS 128u
#define SOURCE_ROOT_CONTROL_SUSPEND UINT64_C(1)
#define SOURCE_ROOT_CONTROL_RESUME UINT64_C(2)
#define SOURCE_ACTOR_CONTROL_OUTPUT_RESUME UINT64_C(3)

typedef struct SourceActor SourceActor;
struct SourceActor {
    uint64_t magic;
    uint64_t id;
    OresActorI64Heap* heap;
    OresNativeActorRef ref;
    OresSourceActorInit constructor;
    OresSourceActorMessage on_message;
    OresNativeActorRef capabilities[ORES_SOURCE_ACTOR_CAPABILITY_SLOTS];
    struct SourceActor* children[ORES_SOURCE_ACTOR_CAPABILITY_SLOTS];
    struct SourceActor* parent;
    int64_t outputs[SOURCE_ACTOR_OUTPUT_CAPACITY];
    uint8_t capability_count;
    uint8_t child_count;
    uint8_t output_head;
    uint8_t output_tail;
    uint8_t output_count;
    OresSourceActorOutputContinuation output_continuation;
    struct SourceActor* output_waiter;
    struct SourceActor* output_wait_child;
    OresNativeSuspensionLink output_wait_link;
    int64_t output_wait_mail;
    int64_t output_wait_value;
    uint64_t output_wait_source_token;
    uint8_t output_waiting;
    uint8_t output_wait_ready;
    atomic_int initialized;
    atomic_int end_requested;
    atomic_uint_fast64_t processed;
    atomic_int_fast64_t last_result;
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
    int64_t result;
} ReadyAwaitFrame;

typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t output_ready;
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
static SourceActorGroup group = {
    .mutex = PTHREAD_MUTEX_INITIALIZER,
    .output_ready = PTHREAD_COND_INITIALIZER
};
static _Atomic uint64_t next_actor_id = 4100;
static _Thread_local OresNativeExecutionHandle current_source_execution;
static _Thread_local SourceActor* current_source_actor;
static _Thread_local uint8_t current_source_phase;

static void ensure(int condition) { if (!condition) abort(); }
static void group_lock(void) { ensure(pthread_mutex_lock(&group.mutex) == 0); }
static void group_unlock(void) { ensure(pthread_mutex_unlock(&group.mutex) == 0); }
static void scrub_bytes(void* address, size_t length) {
    volatile unsigned char* bytes=(volatile unsigned char*)address;
    for (size_t i=0; i<length; ++i) bytes[i]=0;
}

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
static int pop_output_locked(SourceActor* actor, int64_t* out) {
    ensure(actor && out);
    if (actor->output_count == 0) return 0;
    *out = actor->outputs[actor->output_head];
    actor->outputs[actor->output_head] = 0;
    actor->output_head = (uint8_t)(
        (actor->output_head + 1u) % SOURCE_ACTOR_OUTPUT_CAPACITY);
    actor->output_count--;
    return 1;
}
static void publish_output_resume_locked(
        SourceActor* waiter, SourceActor* child, int64_t value) {
    ensure(waiter && child);
    ensure(waiter->output_waiting && !waiter->output_wait_ready);
    ensure(waiter->output_wait_child == child);
    ensure(child->output_waiter == waiter);
    child->output_waiter = NULL;
    waiter->output_wait_value = value;
    waiter->output_wait_ready = 1;
    ensure(ores_native_vm_publish_control(
        group.vm,
        ORES_NATIVE_VM_MAIN,
        waiter->ref,
        SOURCE_ACTOR_CONTROL_OUTPUT_RESUME));
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

static void finish_source_handler_segment(
        SourceActor* instance, int64_t result, int count_message) {
    if (atomic_exchange_explicit(
            &instance->end_requested, 0, memory_order_acq_rel) != 0) {
        /* Mark terminal only. This is carrier-safe and never waits. */
        (void)ores_native_vm_stop(
            group.vm, ORES_NATIVE_VM_MAIN, instance->ref);
        group_lock();
        ensure(pthread_cond_broadcast(&group.output_ready) == 0);
        group_unlock();
    }
    atomic_store_explicit(
        &instance->last_result, result, memory_order_relaxed);
    if (count_message) {
        atomic_fetch_add_explicit(
            &instance->processed, 1, memory_order_release);
    }
}

static void resume_output_turn(SourceActor* instance) {
    OresSourceActorOutputContinuation continuation;
    OresNativeSuspensionLink link;
    int64_t original_mail;
    int64_t output_value;

    group_lock();
    ensure(instance && instance->magic == SOURCE_ACTOR_MAGIC);
    ensure(instance->output_waiting && instance->output_wait_ready);
    continuation = instance->output_continuation;
    link = instance->output_wait_link;
    original_mail = instance->output_wait_mail;
    output_value = instance->output_wait_value;
    ensure(continuation != NULL);
    group_unlock();

    /* CONTROL owns this actor lane, so DATA can be ungated now without
     * permitting a concurrent handler before this continuation returns. */
    ensure(ores_native_vm_resume_current_user_mailbox(
        group.vm, ORES_NATIVE_VM_MAIN));

    OresNativeExecutionContext physical = {0};
    ensure(ores_native_vm_current_execution_context(
        group.vm, ORES_NATIVE_VM_MAIN, &physical));
    ensure(physical.allocation.kind == ORES_ALLOCATION_DOMAIN_ACTOR);
    ensure(physical.allocation.owner == instance->ref.actor_slot);

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

    OresActorTurn turn =
        ores_actor_i64_heap_enter(instance->heap, instance->id, 1);
    ensure(current_source_actor == NULL && current_source_phase == 0);
    current_source_execution = resumed;
    current_source_actor = instance;
    current_source_phase = 2;
    const int64_t result = continuation(
        instance->heap,
        turn.domain,
        turn.epoch,
        original_mail,
        output_value);
    current_source_phase = 0;
    current_source_actor = NULL;
    current_source_execution = (OresNativeExecutionHandle){0, 0};
    ores_actor_i64_heap_leave(instance->heap, turn);

    finish_source_handler_segment(instance, result, 0);
    ensure(ores_native_await_trace_resume_finish(
        group.continuations, link));
    ensure(ores_native_execution_context_store_release(
        group.executions, resumed));
    ensure(ores_native_continuation_store_release(
        group.continuations, link.continuation));

    group_lock();
    ensure(instance->output_waiting && instance->output_wait_ready);
    instance->output_continuation = NULL;
    instance->output_wait_child = NULL;
    instance->output_wait_mail = 0;
    instance->output_wait_value = 0;
    instance->output_wait_source_token = 0;
    instance->output_waiting = 0;
    instance->output_wait_ready = 0;
    instance->output_wait_link = (OresNativeSuspensionLink){0};
    ensure(pthread_cond_broadcast(&group.output_ready) == 0);
    group_unlock();
#ifdef ORESLANG_SOURCE_ACTOR_TESTING
    fprintf(stderr,
        "SOURCE_ACTOR_OUTPUT_AWAIT_TRACE_PASS actor_slot=%llu fresh_resume=1\n",
        (unsigned long long)instance->ref.actor_slot);
#endif
}

static void initialize(void* raw, size_t slot, uint64_t payload) {
    ensure(raw == &group);
    if (slot == SOURCE_GROUP_ROOT_SLOT) {
        if (payload == SOURCE_ROOT_CONTROL_SUSPEND) root_suspend_turn();
        else if (payload == SOURCE_ROOT_CONTROL_RESUME) root_resume_turn();
        else abort();
        return;
    }

    SourceActor* instance = lookup(slot);
    if (payload == SOURCE_ACTOR_CONTROL_OUTPUT_RESUME) {
        ensure(atomic_load_explicit(
            &instance->initialized, memory_order_acquire) == 1);
        resume_output_turn(instance);
        return;
    }
    ensure(payload == 0);
    OresActorTurn turn =
        ores_actor_i64_heap_enter(instance->heap, instance->id, 1);
    ensure(current_source_actor == NULL && current_source_phase == 0);
    current_source_actor = instance;
    current_source_phase = 1;
    instance->constructor(instance->heap, turn.domain, turn.epoch);
    current_source_phase = 0;
    current_source_actor = NULL;
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

    OresNativeExecutionContext physical = {0};
    ensure(ores_native_vm_current_execution_context(
        group.vm, ORES_NATIVE_VM_MAIN, &physical));
    ensure(physical.allocation.kind == ORES_ALLOCATION_DOMAIN_ACTOR);
    ensure(physical.allocation.owner == instance->ref.actor_slot);
    OresNativeExecutionHandle execution = {0, 0};
    ensure(ores_native_execution_context_store_insert(
        group.executions, &physical, &execution));

    OresActorTurn turn =
        ores_actor_i64_heap_enter(instance->heap, instance->id, 1);
    ensure(current_source_actor == NULL && current_source_phase == 0);
    ensure(!ores_native_store_handle_valid(current_source_execution));
    current_source_execution = execution;
    current_source_actor = instance;
    current_source_phase = 2;
    const int64_t result = instance->on_message(
        instance->heap, turn.domain, turn.epoch, (int64_t)payload);
    current_source_phase = 0;
    current_source_actor = NULL;
    current_source_execution = (OresNativeExecutionHandle){0, 0};
    ores_actor_i64_heap_leave(instance->heap, turn);

    finish_source_handler_segment(instance, result, 1);
    /* A published await continuation retains its own durable pin. */
    ensure(ores_native_execution_context_store_release(
        group.executions, execution));
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

static SourceActor* spawn_actor_locked(
        OresSourceActorInit constructor,
        OresSourceActorMessage on_message,
        uint64_t fields,
        const OresNativeActorRef* copied,
        uint64_t actor_count,
        SourceActor* parent) {
    ensure(constructor != NULL && fields >= 1 && fields <= 4096);
    ensure(actor_count <= ORES_SOURCE_ACTOR_CAPABILITY_SLOTS);
    ensure(actor_count == 0 || copied != NULL);
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
    instance->capability_count = (uint8_t)actor_count;
    instance->parent = parent;
    for (uint64_t i = 0; i < actor_count; ++i)
        instance->capabilities[i] = copied[i];
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
    return instance;
}

void* oreslang_source_actor_spawn_with_capabilities(
        OresSourceActorInit constructor,
        OresSourceActorMessage on_message,
        uint64_t fields,
        void* const* actors,
        uint64_t actor_count) {
    ensure(actor_count <= ORES_SOURCE_ACTOR_CAPABILITY_SLOTS);
    ensure(actor_count == 0 || actors != NULL);
    group_lock();
    OresNativeActorRef copied[ORES_SOURCE_ACTOR_CAPABILITY_SLOTS] = {0};
    for (uint64_t i = 0; i < actor_count; ++i) {
        SourceActor* target = NULL;
        size_t target_slot = 0;
        for (size_t candidate = 0; candidate < group.next_slot; ++candidate) {
            if (group.actors[candidate] == actors[i]) {
                target = group.actors[candidate];
                target_slot = candidate;
                break;
            }
        }
        ensure(target && target->magic == SOURCE_ACTOR_MAGIC);
        ensure(target->ref.scheduler_domain
            && target->ref.actor_slot == target_slot);
        copied[i] = target->ref;
    }
    SourceActor* instance = spawn_actor_locked(
        constructor, on_message, fields, copied, actor_count, NULL);
    group_unlock();
    return instance;
}

void* oreslang_source_actor_spawn(
        OresSourceActorInit constructor,
        OresSourceActorMessage on_message,
        uint64_t fields) {
    return oreslang_source_actor_spawn_with_capabilities(
        constructor, on_message, fields, NULL, 0);
}

static OresNativeSendStatus source_actor_send_ref(
        OresNativeActorRef ref, int64_t payload) {
    ensure(group.vm != NULL && ref.scheduler_domain != 0);
    return ores_native_vm_try_send(
        group.vm, ORES_NATIVE_VM_MAIN, ref, (uint64_t)payload);
}

static int source_actor_try_send_ref(
        OresNativeActorRef ref, int64_t payload) {
    const OresNativeSendStatus status = source_actor_send_ref(ref, payload);
    if (status == ORES_NATIVE_SEND_ACCEPTED) return 1;
    if (status == ORES_NATIVE_SEND_MAILBOX_FULL) return 0;
    ensure(0);
    return 0;
}

void oreslang_source_actor_send(void* opaque, int64_t payload) {
    SourceActor* instance = (SourceActor*)opaque;
    ensure(instance && instance->magic == SOURCE_ACTOR_MAGIC
        && instance->on_message);
    ensure(source_actor_send_ref(instance->ref, payload)
        == ORES_NATIVE_SEND_ACCEPTED);
}

int oreslang_source_actor_try_send(void* opaque, int64_t payload) {
    SourceActor* instance = (SourceActor*)opaque;
    ensure(instance && instance->magic == SOURCE_ACTOR_MAGIC
        && instance->on_message);
    return source_actor_try_send_ref(instance->ref, payload);
}

static OresNativeActorRef current_capability(
        OresActorI64Heap* heap, uint64_t slot) {
    SourceActor* instance = current_source_actor;
    ensure(instance && instance->magic == SOURCE_ACTOR_MAGIC);
    ensure(current_source_phase == 2);
    ensure(instance->heap == heap);
    ensure(slot < instance->capability_count);
    OresNativeActorRef ref = instance->capabilities[slot];
    ensure(ref.scheduler_domain != 0);
    return ref;
}

void oreslang_source_actor_send_capability(
        OresActorI64Heap* heap, uint64_t slot, int64_t payload) {
    ensure(source_actor_send_ref(current_capability(heap, slot), payload)
        == ORES_NATIVE_SEND_ACCEPTED);
}

int oreslang_source_actor_try_send_capability(
        OresActorI64Heap* heap, uint64_t slot, int64_t payload) {
    return source_actor_try_send_ref(current_capability(heap, slot), payload);
}

void oreslang_source_actor_emit_output(
        OresActorI64Heap* heap, int64_t payload) {
    SourceActor* instance = current_source_actor;
    ensure(instance && instance->magic == SOURCE_ACTOR_MAGIC);
    ensure(current_source_phase == 2 && instance->heap == heap);

    group_lock();
    if (instance->output_waiter) {
        SourceActor* waiter = instance->output_waiter;
        publish_output_resume_locked(waiter, instance, payload);
    } else {
        ensure(instance->output_count < SOURCE_ACTOR_OUTPUT_CAPACITY);
        instance->outputs[instance->output_tail] = payload;
        instance->output_tail = (uint8_t)(
            (instance->output_tail + 1u) % SOURCE_ACTOR_OUTPUT_CAPACITY);
        instance->output_count++;
    }
    ensure(pthread_cond_broadcast(&group.output_ready) == 0);
    group_unlock();
}

int64_t oreslang_source_actor_take_output(void* opaque) {
    ensure(current_source_actor == NULL);
    if (group.vm) {
        OresNativeExecutionContext carrier={0};
        ensure(!ores_native_vm_current_execution_context(
            group.vm, ORES_NATIVE_VM_MAIN, &carrier));
    }
    group_lock();
    SourceActor* instance = NULL;
    for (size_t i = 0; i < group.next_slot; ++i) {
        if (group.actors[i] == opaque) {
            instance = group.actors[i];
            break;
        }
    }
    ensure(instance && instance->magic == SOURCE_ACTOR_MAGIC);
    while (instance->output_count == 0) {
        if (ores_native_vm_poll_done(
                group.vm, ORES_NATIVE_VM_MAIN, instance->ref)) {
            group_unlock();
            abort();
        }
        ensure(pthread_cond_wait(&group.output_ready, &group.mutex) == 0);
    }
    int64_t value = 0;
    ensure(pop_output_locked(instance, &value));
    group_unlock();
    return value;
}

void oreslang_source_actor_stdout_i64(int64_t value) {
    OresNativeExecutionContext carrier={0};
    if (group.vm)
        ensure(!ores_native_vm_current_execution_context(
            group.vm, ORES_NATIVE_VM_MAIN, &carrier));
    ensure(fprintf(stdout, "%lld", (long long)value) >= 0);
    ensure(fflush(stdout) == 0);
}

void oreslang_source_actor_request_end(OresActorI64Heap* heap) {
    SourceActor* instance = current_source_actor;
    ensure(instance && instance->magic == SOURCE_ACTOR_MAGIC);
    ensure(current_source_phase == 2 && instance->heap == heap);
    atomic_store_explicit(&instance->end_requested, 1, memory_order_release);
}

void oreslang_source_actor_await_child_output(
        OresActorI64Heap* heap,
        uint64_t capability,
        int64_t original_mail,
        OresSourceActorOutputContinuation continuation,
        uint64_t source_token) {
    SourceActor* waiter = current_source_actor;
    ensure(waiter && waiter->magic == SOURCE_ACTOR_MAGIC);
    ensure(current_source_phase == 2 && waiter->heap == heap);
    ensure(continuation != NULL);
    ensure(capability < waiter->child_count);
    SourceActor* child = waiter->children[capability];
    ensure(child && child->magic == SOURCE_ACTOR_MAGIC);
    ensure(waiter->capabilities[capability].scheduler_domain
        == child->ref.scheduler_domain);
    ensure(waiter->capabilities[capability].actor_slot
        == child->ref.actor_slot);
    ensure(waiter->capabilities[capability].generation
        == child->ref.generation);
    ensure(!waiter->output_waiting && child->output_waiter == NULL);
    ensure(ores_native_store_handle_valid(current_source_execution));

    OresNativeContinuationHandle no_parent = {0, 0};
    OresNativeSuspensionLink link = {0};
    ensure(ores_native_await_trace_suspend(
        group.executions,
        group.continuations,
        current_source_execution,
        no_parent,
        ORES_CONTINUATION_BOUNDARY_AWAIT,
        1,
        source_token,
        &link));
    ensure(ores_native_vm_suspend_current_user_mailbox(
        group.vm, ORES_NATIVE_VM_MAIN));

    group_lock();
    waiter->output_continuation = continuation;
    waiter->output_wait_child = child;
    waiter->output_wait_link = link;
    waiter->output_wait_mail = original_mail;
    waiter->output_wait_source_token = source_token;
    waiter->output_waiting = 1;
    waiter->output_wait_ready = 0;
    child->output_waiter = waiter;

    int64_t already_ready = 0;
    if (pop_output_locked(child, &already_ready))
        publish_output_resume_locked(waiter, child, already_ready);
    group_unlock();
}

void oreslang_source_actor_spawn_child_capability(
        OresActorI64Heap* heap,
        uint64_t slot,
        OresSourceActorInit constructor,
        OresSourceActorMessage on_message,
        uint64_t fields) {
    SourceActor* parent = current_source_actor;
    ensure(parent && parent->magic == SOURCE_ACTOR_MAGIC);
    ensure(current_source_phase == 1 && parent->heap == heap);
    ensure(slot == parent->capability_count);
    ensure(slot == parent->child_count);
    ensure(slot < ORES_SOURCE_ACTOR_CAPABILITY_SLOTS);

    group_lock();
    SourceActor* child = spawn_actor_locked(
        constructor, on_message, fields, NULL, 0, parent);
    ensure(child != NULL);
    parent->capabilities[slot] = child->ref;
    parent->children[slot] = child;
    parent->capability_count++;
    parent->child_count++;
    group_unlock();
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

static void finish_source_actor(void* opaque, int request_stop) {
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
    while (instance->output_waiting) {
        SourceActor* awaited_child=instance->output_wait_child;
        if (awaited_child && ores_native_vm_poll_done(
                group.vm, ORES_NATIVE_VM_MAIN, awaited_child->ref)
                && !instance->output_wait_ready) {
            group_unlock();
            abort();
        }
        ensure(pthread_cond_wait(&group.output_ready, &group.mutex) == 0);
        ensure(group.actors[slot] == instance);
    }
    if (request_stop) {
        /* If self.end() already retired the lane, stop returns zero. The ref
         * was validated above, so either state is safe to hand to wait_done. */
        (void)ores_native_vm_stop(
            group.vm, ORES_NATIVE_VM_MAIN, instance->ref);
    }
    group_unlock();

    ensure(ores_native_vm_wait_done(
        group.vm, ORES_NATIVE_VM_MAIN, instance->ref));

    SourceActor* children[ORES_SOURCE_ACTOR_CAPABILITY_SLOTS] = {0};
    uint8_t child_count = 0;
    group_lock();
    ensure(group.actors[slot] == instance);
    child_count = instance->child_count;
    for (uint8_t i = 0; i < child_count; ++i)
        children[i] = instance->children[i];
    group_unlock();

    for (uint8_t i = 0; i < child_count; ++i) {
        if (children[i]) oreslang_source_actor_stop(children[i]);
    }

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
        "SOURCE_ACTOR_SPAWN_STOP_VERIFIED constructor_on_worker=1 released=1 requested_stop=%d processed=%llu last=%lld outputs=%u\n",
        request_stop,
        (unsigned long long)atomic_load_explicit(
            &instance->processed, memory_order_acquire),
        (long long)atomic_load_explicit(
            &instance->last_result, memory_order_relaxed),
        (unsigned)instance->output_count);
#endif
    group.actors[slot] = NULL;
    scrub_bytes(instance,sizeof(*instance));
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


void oreslang_source_actor_stop(void* opaque) {
    finish_source_actor(opaque, 1);
}

void oreslang_source_actor_await_done(void* opaque) {
    finish_source_actor(opaque, 0);
}
