#include "oreslang/runtime/SourceActorBootstrap.h"
#include "oreslang/runtime/NativeActorScheduler.h"
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#define SOURCE_ACTOR_MAGIC UINT64_C(0x4f52455353504157)
typedef struct SourceActor {
    uint64_t magic;
    uint64_t id;
    OresActorI64Heap* heap;
    OresNativeActorScheduler* scheduler;
    OresNativeActorRef ref;
    OresSourceActorInit constructor;
    OresSourceActorMessage on_message;
    atomic_int initialized;
    atomic_uint_fast64_t processed;
    atomic_int_fast64_t last_result;
    _Atomic(struct SourceActor*) downstream;
    _Atomic(struct SourceActor*) upstream;
    atomic_uint_fast64_t forward_reserved;
} SourceActor;

static _Atomic uint64_t next_actor_id = 4100;
static void ensure(int condition) { if (!condition) abort(); }
static void initialize(void* raw, size_t actor, uint64_t payload) {
    SourceActor* instance = (SourceActor*)raw;
    ensure(instance && instance->magic == SOURCE_ACTOR_MAGIC);
    ensure(actor == 0 && payload == 0);
    OresActorTurn turn = ores_actor_i64_heap_enter(instance->heap, instance->id, 1);
    instance->constructor(instance->heap, turn.domain, turn.epoch);
    ores_actor_i64_heap_leave(instance->heap, turn);
    atomic_store_explicit(&instance->initialized, 1, memory_order_release);
}
static void deliver(void* raw, size_t actor, uint64_t payload) {
    SourceActor* instance = (SourceActor*)raw;
    ensure(instance && instance->magic == SOURCE_ACTOR_MAGIC && actor == 0);
    ensure(instance->on_message != NULL);
    ensure(atomic_load_explicit(&instance->initialized, memory_order_acquire) == 1);
    OresActorTurn turn = ores_actor_i64_heap_enter(instance->heap, instance->id, 1);
    const int64_t result = instance->on_message(
        instance->heap, turn.domain, turn.epoch, (int64_t)payload);
    ores_actor_i64_heap_leave(instance->heap, turn);
    atomic_store_explicit(&instance->last_result, result, memory_order_relaxed);
    atomic_fetch_add_explicit(&instance->processed, 1, memory_order_release);
    // This is an admitted producer DATA turn. Routing is nonblocking; a
    // reservation was made BEFORE the source message entered its mailbox.
    SourceActor* sink = atomic_load_explicit(&instance->downstream, memory_order_acquire);
    if (sink != NULL) {
        ensure(ores_native_actor_scheduler_send(
            sink->scheduler, sink->ref, (uint64_t)result));
    }
    SourceActor* producer = atomic_load_explicit(&instance->upstream, memory_order_acquire);
    if (producer != NULL) {
        const uint64_t prior = atomic_fetch_sub_explicit(
            &producer->forward_reserved, 1, memory_order_acq_rel);
        ensure(prior > 0);
    }
}
void* oreslang_source_actor_spawn(OresSourceActorInit constructor,
                                  OresSourceActorMessage on_message,
                                  uint64_t fields) {
    ensure(constructor != NULL && fields >= 1 && fields <= 4096);
    SourceActor* instance = (SourceActor*)calloc(1, sizeof(*instance));
    ensure(instance != NULL);
    instance->magic = SOURCE_ACTOR_MAGIC;
    instance->id = atomic_fetch_add_explicit(&next_actor_id, 1, memory_order_relaxed);
    ensure(instance->id != 0 && instance->id != UINT64_MAX);
    instance->constructor = constructor;
    instance->on_message = on_message;
    instance->heap = ores_actor_i64_heap_open(instance->id, 1, fields);
    instance->scheduler = ores_native_actor_scheduler_open_control(
        1, 128, 1, deliver, initialize, instance);
    ensure(instance->scheduler != NULL);
    instance->ref = ores_native_actor_scheduler_spawn(instance->scheduler, 0);
    ensure(instance->ref.scheduler_domain != 0);
    // Initializer runs in reserved CONTROL, which the scheduler admits
    // ahead of ordinary messages without blocking a source submitter.
    ensure(ores_native_actor_scheduler_publish_control(
        instance->scheduler, instance->ref, 0));
    /* Admission returned immediately. The compiler-generated source main is
     * not a carrier; an explicit trusted stop provides a teardown barrier.
     * There is NO source await here and no claim of async get_ready()/get_done().
     */
    return instance;
}
void oreslang_source_actor_pipe(void* upstream, void* downstream) {
    SourceActor* producer = (SourceActor*)upstream;
    SourceActor* consumer = (SourceActor*)downstream;
    ensure(producer && consumer && producer != consumer
        && producer->magic == SOURCE_ACTOR_MAGIC
        && consumer->magic == SOURCE_ACTOR_MAGIC
        && producer->on_message && consumer->on_message);
    // A single, acyclic, main-owned i64 edge. Ref/actor ownership never
    // crosses between actor-owned heaps and no payload aliases mutable memory.
    ensure(atomic_load_explicit(&producer->downstream, memory_order_acquire) == NULL);
    ensure(atomic_load_explicit(&producer->upstream, memory_order_acquire) == NULL);
    ensure(atomic_load_explicit(&consumer->upstream, memory_order_acquire) == NULL);
    ensure(atomic_load_explicit(&consumer->downstream, memory_order_acquire) == NULL);
    atomic_store_explicit(&consumer->upstream, producer, memory_order_release);
    atomic_store_explicit(&producer->downstream, consumer, memory_order_release);
}

int oreslang_source_actor_try_send(void* opaque, int64_t payload) {
    SourceActor* actor = (SourceActor*)opaque;
    if (!actor || actor->magic != SOURCE_ACTOR_MAGIC || !actor->on_message)
        return 0;
    // A downstream-bound actor is not also a public producer. Mixed writers
    // would invalidate the one-to-one forwarding quota proof.
    if (atomic_load_explicit(&actor->upstream, memory_order_acquire) != NULL)
        return 0;
    const bool forwarding =
        atomic_load_explicit(&actor->downstream, memory_order_acquire) != NULL;
    if (forwarding) {
        // At most 128 accepted but not yet sink-acknowledged messages. There
        // cannot be downstream saturation if this is the only writer.
        uint_fast64_t used = atomic_load_explicit(&actor->forward_reserved, memory_order_acquire);
        do {
            if (used >= 128) return 0;
        } while (!atomic_compare_exchange_weak_explicit(
            &actor->forward_reserved, &used, used + 1,
            memory_order_acq_rel, memory_order_acquire));
    }
    if (ores_native_actor_scheduler_send(
            actor->scheduler, actor->ref, (uint64_t)payload))
        return 1;
    if (forwarding) {
        const uint64_t prior = atomic_fetch_sub_explicit(
            &actor->forward_reserved, 1, memory_order_acq_rel);
        ensure(prior > 0);
    }
    return 0;
}

void oreslang_source_actor_send(void* opaque, int64_t payload) {
    // Source send is still a void statement in the tiny guest compiler.
    // An overload cannot be acknowledged without a typed return: refuse
    // loss rather than silently dropping or blocking a native carrier.
    ensure(oreslang_source_actor_try_send(opaque, payload));
}
void oreslang_source_actor_stop(void* opaque) {
    SourceActor* instance = (SourceActor*)opaque;
    ensure(instance && instance->magic == SOURCE_ACTOR_MAGIC);
    ensure(atomic_load_explicit(&instance->upstream, memory_order_acquire) == NULL);
    ensure(ores_native_actor_scheduler_drain(instance->scheduler));
    // The source has stopped producing; after its own drain, every accepted
    // forwarded result has been admitted to the consumer scheduler. This is
    // a HOST stop barrier, never an actor continuation/carrier wait.
    SourceActor* sink = atomic_load_explicit(&instance->downstream, memory_order_acquire);
    if (sink != NULL) {
        ensure(ores_native_actor_scheduler_drain(sink->scheduler));
        ensure(atomic_load_explicit(&instance->forward_reserved, memory_order_acquire) == 0);
        atomic_store_explicit(&sink->upstream, NULL, memory_order_release);
        atomic_store_explicit(&instance->downstream, NULL, memory_order_release);
    }
    ensure(atomic_load_explicit(&instance->initialized, memory_order_acquire) == 1);
    ensure(ores_native_actor_scheduler_stop(instance->scheduler, instance->ref));
    ensure(ores_native_actor_scheduler_poll_done(instance->scheduler, instance->ref));
    ores_native_actor_scheduler_retire(instance->scheduler);
    ensure(ores_native_actor_scheduler_try_destroy(instance->scheduler));
    ores_actor_i64_heap_retire(instance->heap);
    ensure(ores_actor_i64_heap_try_destroy(instance->heap));
#ifdef ORESLANG_SOURCE_ACTOR_TESTING
    fprintf(stderr,
        "SOURCE_ACTOR_SPAWN_STOP_VERIFIED constructor_on_worker=1 released=1 processed=%llu last=%lld\n",
        (unsigned long long)atomic_load_explicit(&instance->processed, memory_order_acquire),
        (long long)atomic_load_explicit(&instance->last_result, memory_order_relaxed));
#endif
    instance->magic = 0;
    free(instance);
}
