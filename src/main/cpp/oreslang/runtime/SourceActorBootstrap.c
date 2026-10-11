#include "oreslang/runtime/SourceActorBootstrap.h"
#include "oreslang/runtime/NativeActorScheduler.h"
#include "oreslang/runtime/NativeActorFuture.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SOURCE_ACTOR_MAGIC UINT64_C(0x4f52455353504157)
#define SOURCE_GROUP_SLOTS 64u
#define SOURCE_GROUP_CARRIERS 4u
#define SOURCE_GROUP_MAILBOX 128u
#define SOURCE_GROUP_ROOT_SLOT (SOURCE_GROUP_SLOTS - 1u)

typedef struct {
    uint64_t magic;
    uint64_t id;
    OresActorI64Heap* heap;
    OresNativeActorRef ref;
    OresSourceActorInit constructor;
    OresSourceActorMessage on_message;
    OresSourceActorWireMessage on_wire;
    OresMp2Contract wire_contract;
    char* owned_schema;
    char* owned_shape;
    uint8_t wire_sha[32];
    atomic_size_t wire_reserved;
    atomic_int initialized;
    atomic_uint_fast64_t processed;
    atomic_int_fast64_t last_result;
} SourceActor;

typedef struct {
    pthread_cond_t completed;
    OresNativeActorFuture* ready_future;
    OresSourceActorContinuation continuation;
    SourceActor* actor;
    int done;
    int64_t result;
} ReadyAwaitFrame;

typedef struct {
    pthread_mutex_t mutex;
    OresNativeActorScheduler* scheduler;
    OresNativeActorRef root_ref;
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
static void check_invariant(int condition, const char* predicate, int line) {
    if (!condition) {
        fprintf(stderr, "SOURCE_ACTOR_INVARIANT_FAILURE line=%d check=%s\n",
                line, predicate);
        abort();
    }
}
#define ensure(expr) check_invariant((expr), #expr, __LINE__)
static void group_lock(void) { ensure(pthread_mutex_lock(&group.mutex) == 0); }
static void group_unlock(void) { ensure(pthread_mutex_unlock(&group.mutex) == 0); }

static SourceActor* lookup(size_t index) {
    ensure(index < SOURCE_GROUP_SLOTS);
    SourceActor* actor = group.actors[index];
    ensure(actor && actor->magic == SOURCE_ACTOR_MAGIC);
    return actor;
}
static void initialize(void* raw, size_t slot, uint64_t payload) {
    ensure(raw == &group);
    if (slot == SOURCE_GROUP_ROOT_SLOT) {
        /* Root continuation is admitted as its own exclusive CONTROL turn.
         * It cannot run on the child constructor's carrier inline. */
        ensure(payload == 1);
        group_lock();
        ReadyAwaitFrame* frame = group.pending_ready;
        ensure(frame && !frame->done && frame->ready_future);
        ensure(ores_native_actor_future_status(frame->ready_future)
            == ORES_NATIVE_FUTURE_READY);
        OresSourceActorContinuation continuation = frame->continuation;
        SourceActor* actor = frame->actor;
        group_unlock();
        const int64_t result = continuation(actor);
        group_lock();
        frame->result = result;
        frame->done = 1;
#ifdef ORESLANG_SOURCE_ACTOR_TESTING
        fputs("SOURCE_AWAIT_READY_CONTINUATION_PASS root_lane=1 real_future=1\n",
            stderr);
#endif
        ensure(pthread_cond_signal(&frame->completed) == 0);
        group_unlock();
        return;
    }
    ensure(payload == 0);
    SourceActor* instance = lookup(slot);
    OresActorTurn turn = ores_actor_i64_heap_enter(instance->heap, instance->id, 1);
    instance->constructor(instance->heap, turn.domain, turn.epoch);
    ores_actor_i64_heap_leave(instance->heap, turn);
    atomic_store_explicit(&instance->initialized, 1, memory_order_release);
    group_lock();
    /* Registration may race with initialization. Both paths serialize via
     * group.mutex and settle exactly once through NativeActorFuture. */
    ReadyAwaitFrame* frame = group.pending_ready;
    if (frame && frame->actor == instance
        && ores_native_actor_future_status(frame->ready_future)
            == ORES_NATIVE_FUTURE_PENDING) {
        /* Registration can observe initialized=1 before the initializer
         * acquires group.mutex. Only the still-pending Future gets settled;
         * an already-published READY wake must never be completed twice. */
        ensure(ores_native_actor_future_complete(
            frame->ready_future, ORES_NATIVE_FUTURE_READY));
    }
    group_unlock();
}
static void deliver(void* raw, size_t slot, uint64_t payload) {
    ensure(raw == &group);
    SourceActor* instance = lookup(slot);
    ensure((instance->on_message != NULL) != (instance->on_wire != NULL));
    ensure(atomic_load_explicit(&instance->initialized, memory_order_acquire) == 1);
    OresActorTurn turn = ores_actor_i64_heap_enter(instance->heap, instance->id, 1);
    int64_t result;
    if (instance->on_wire) {
        OresMp2Owned* accepted = (OresMp2Owned*)(uintptr_t)payload;
        ensure(accepted && accepted->bytes && accepted->size);
        result = instance->on_wire(instance->heap, turn.domain, turn.epoch,
                                   accepted->bytes, accepted->size);
        ores_actor_i64_heap_leave(instance->heap, turn);
        ores_mp2_owned_release(accepted);
        free(accepted);
        size_t before = atomic_fetch_sub_explicit(
            &instance->wire_reserved, 1, memory_order_acq_rel);
        ensure(before > 0);
    } else {
        result = instance->on_message(instance->heap, turn.domain,
                                      turn.epoch, (int64_t)payload);
        ores_actor_i64_heap_leave(instance->heap, turn);
    }
    atomic_store_explicit(&instance->last_result, result, memory_order_relaxed);
    atomic_fetch_add_explicit(&instance->processed, 1, memory_order_release);
}
/* Returns zero only for invalid trusted registration. Configuration and
 * teardown are single-owner operations; concurrent host senders must be
 * fenced before stop to avoid use-after-free of the opaque actor handle.
 */
static void* source_spawn(
    OresSourceActorInit constructor, OresSourceActorMessage on_message,
    OresSourceActorWireMessage on_wire, uint64_t fields,
    const OresMp2Contract* contract) {
    ensure(constructor != NULL && fields >= 1 && fields <= 4096);
    ensure((on_message != NULL) != (on_wire != NULL));
    if (on_wire && (!contract || !ores_mp2_contract_valid(contract)))
        return NULL;
    group_lock();
    if (!group.scheduler) {
        ensure(group.live == 0 && group.next_slot == 0 && group.peak == 0
            && group.pending_ready == NULL);
        group.scheduler = ores_native_actor_scheduler_open_control(
            SOURCE_GROUP_SLOTS, SOURCE_GROUP_MAILBOX, SOURCE_GROUP_CARRIERS,
            deliver, initialize, &group);
        ensure(group.scheduler != NULL);
        group.root_ref = ores_native_actor_scheduler_spawn(
            group.scheduler, SOURCE_GROUP_ROOT_SLOT);
        ensure(group.root_ref.scheduler_domain != 0);
    }
    ensure(group.next_slot < SOURCE_GROUP_ROOT_SLOT);
    const size_t slot = group.next_slot++;
    SourceActor* instance = (SourceActor*)calloc(1, sizeof(*instance));
    ensure(instance != NULL);
    instance->magic = SOURCE_ACTOR_MAGIC;
    instance->id = atomic_fetch_add_explicit(&next_actor_id, 1, memory_order_relaxed);
    ensure(instance->id != 0 && instance->id != UINT64_MAX);
    instance->constructor = constructor;
    instance->on_message = on_message;
    instance->on_wire = on_wire;
    atomic_init(&instance->wire_reserved, 0);
    if (on_wire) {
        const size_t schema_size = strlen(contract->schema_id) + 1;
        const size_t shape_size = strlen(contract->shape) + 1;
        instance->owned_schema = malloc(schema_size);
        instance->owned_shape = malloc(shape_size);
        ensure(instance->owned_schema && instance->owned_shape);
        memcpy(instance->owned_schema, contract->schema_id, schema_size);
        memcpy(instance->owned_shape, contract->shape, shape_size);
        memcpy(instance->wire_sha, contract->shape_sha256, 32);
        instance->wire_contract = (OresMp2Contract){
            instance->owned_schema, instance->owned_shape, instance->wire_sha
        };
    }
    instance->heap = ores_actor_i64_heap_open(instance->id, 1, fields);
    group.actors[slot] = instance;
    instance->ref = ores_native_actor_scheduler_spawn(group.scheduler, slot);
    ensure(instance->ref.scheduler_domain && instance->ref.actor_slot == slot);
    group.live++;
    if (group.live > group.peak) group.peak = group.live;
    /* CONTROL initializes the actual compiler-emitted actor constructor
     * ahead of normal messages, without blocking the submitting carrier. */
    ensure(ores_native_actor_scheduler_publish_control(
        group.scheduler, instance->ref, 0));
    group_unlock();
    return instance;
}
void* oreslang_source_actor_spawn(OresSourceActorInit constructor,
                                  OresSourceActorMessage on_message,
                                  uint64_t fields) {
    return source_spawn(constructor, on_message, NULL, fields, NULL);
}
void* oreslang_source_actor_spawn_wire(
    OresSourceActorInit constructor, OresSourceActorWireMessage on_wire,
    uint64_t fields, const OresMp2Contract* contract) {
    if (!on_wire || !contract) return NULL;
    return source_spawn(constructor, NULL, on_wire, fields, contract);
}
int64_t oreslang_source_actor_await_ready(
    void* opaque, OresSourceActorContinuation continuation) {
    SourceActor* actor = (SourceActor*)opaque;
    ensure(actor && actor->magic == SOURCE_ACTOR_MAGIC && continuation);
    group_lock();
    ensure(group.scheduler && !group.pending_ready);
    const size_t slot = (size_t)actor->ref.actor_slot;
    ensure(slot < group.next_slot && group.actors[slot] == actor);
    ReadyAwaitFrame frame = {0};
    ensure(pthread_cond_init(&frame.completed, NULL) == 0);
    frame.actor = actor;
    frame.continuation = continuation;
    frame.ready_future = ores_native_actor_future_open(
        group.scheduler, group.root_ref, 1);
    ensure(frame.ready_future != NULL);
    group.pending_ready = &frame;
    if (atomic_load_explicit(&actor->initialized, memory_order_acquire)) {
        ensure(ores_native_actor_future_complete(
            frame.ready_future, ORES_NATIVE_FUTURE_READY));
    }
    /* External entrypoint barrier only. This caller is never a scheduler
     * worker; pending Future waits consume no ActorGroup carrier thread. */
    while (!frame.done) {
        ensure(pthread_cond_wait(&frame.completed, &group.mutex) == 0);
    }
    ensure(group.pending_ready == &frame);
    group.pending_ready = NULL;
    const int64_t result = frame.result;
    ores_native_actor_future_destroy(frame.ready_future);
    ensure(pthread_cond_destroy(&frame.completed) == 0);
    group_unlock();
    return result;
}
void oreslang_source_actor_send(void* opaque, int64_t payload) {
    SourceActor* instance = (SourceActor*)opaque;
    ensure(instance && instance->magic == SOURCE_ACTOR_MAGIC
        && instance->on_message && !instance->on_wire);
    /* The source language still only permits one trusted-main submitter.
     * Nonblocking bounded sends fail closed on overload. */
    ensure(ores_native_actor_scheduler_send(
        group.scheduler, instance->ref, (uint64_t)payload));
}
int oreslang_source_actor_send_wire(void* opaque,
                                   const uint8_t* wire, size_t size) {
    SourceActor* instance = opaque;
    if (!instance || instance->magic != SOURCE_ACTOR_MAGIC
            || !instance->on_wire || !wire || size > ORES_MP2_MAX_BYTES)
        return 0;
    /* Bound bytes being prepared BEFORE allocating. The reservation survives
     * admission and is returned only after the admitted actor's callback.
     * All messages share this actor's original single-flight DATA lane.
     */
    size_t pending = atomic_load_explicit(&instance->wire_reserved,
                                          memory_order_relaxed);
    do {
        if (pending >= SOURCE_GROUP_MAILBOX) return 0;
    } while (!atomic_compare_exchange_weak_explicit(
        &instance->wire_reserved, &pending, pending + 1,
        memory_order_acq_rel, memory_order_relaxed));
    OresMp2Owned* snapshot = calloc(1, sizeof(*snapshot));
    if (!snapshot) {
        atomic_fetch_sub_explicit(&instance->wire_reserved, 1,
                                  memory_order_acq_rel);
        return 0;
    }
    if (!ores_mp2_validate_copy(&instance->wire_contract, wire, size, snapshot)) {
        free(snapshot);
        atomic_fetch_sub_explicit(&instance->wire_reserved, 1,
                                  memory_order_acq_rel);
        return 0;
    }
    if (!ores_native_actor_scheduler_send(group.scheduler, instance->ref,
                                           (uint64_t)(uintptr_t)snapshot)) {
        ores_mp2_owned_release(snapshot);
        free(snapshot);
        atomic_fetch_sub_explicit(&instance->wire_reserved, 1,
                                  memory_order_acq_rel);
        return 0;
    }
    return 1;
}
void oreslang_source_actor_stop(void* opaque) {
    SourceActor* instance = (SourceActor*)opaque;
    ensure(instance && instance->magic == SOURCE_ACTOR_MAGIC);
    group_lock();
    const size_t slot = (size_t)instance->ref.actor_slot;
    ensure(group.scheduler && slot < group.next_slot && group.actors[slot] == instance);
    ensure(ores_native_actor_scheduler_stop(group.scheduler, instance->ref));
    /* Never hold the ActorGroup mutex during a blocking host join.
     * A finishing initializer itself needs that mutex to publish readiness.
     * Trusted main is the sole source stop owner in this compiler subset. */
    group_unlock();
    ensure(ores_native_actor_scheduler_wait_done(group.scheduler, instance->ref));
    group_lock();
    ensure(group.actors[slot] == instance);
    ensure(atomic_load_explicit(&instance->initialized, memory_order_acquire) == 1);
    ensure(ores_native_actor_scheduler_poll_done(group.scheduler, instance->ref));
    ores_actor_i64_heap_retire(instance->heap);
    ensure(ores_actor_i64_heap_try_destroy(instance->heap));
#ifdef ORESLANG_SOURCE_ACTOR_TESTING
    fprintf(stderr,
        "SOURCE_ACTOR_SPAWN_STOP_VERIFIED constructor_on_worker=1 released=1 processed=%llu last=%lld\n",
        (unsigned long long)atomic_load_explicit(&instance->processed, memory_order_acquire),
        (long long)atomic_load_explicit(&instance->last_result, memory_order_relaxed));
#endif
    group.actors[slot] = NULL;
    ensure(atomic_load_explicit(&instance->wire_reserved,
                                 memory_order_acquire) == 0);
    free(instance->owned_schema);
    free(instance->owned_shape);
    instance->magic = 0;
    free(instance);
    ensure(group.live > 0);
    group.live--;
    if (group.live == 0) {
        ensure(group.pending_ready == NULL);
        /* The root also lives on the shared pool, with an exclusive lane. */
        ensure(ores_native_actor_scheduler_stop(group.scheduler, group.root_ref));
        ensure(ores_native_actor_scheduler_wait_done(group.scheduler, group.root_ref));
        /* All children and root continuations are quiescent. */
        ores_native_actor_scheduler_retire(group.scheduler);
        ensure(ores_native_actor_scheduler_try_destroy(group.scheduler));
        group.scheduler = NULL;
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
