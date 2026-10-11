#include "oreslang/runtime/SourceActorBootstrap.h"
#include "oreslang/runtime/NativeActorScheduler.h"
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#define SOURCE_ACTOR_MAGIC UINT64_C(0x4f52455353504157)
typedef struct {
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
void oreslang_source_actor_send(void* opaque, int64_t payload) {
    SourceActor* instance = (SourceActor*)opaque;
    ensure(instance && instance->magic == SOURCE_ACTOR_MAGIC
        && instance->on_message != NULL);
    ensure(ores_native_actor_scheduler_send(
        instance->scheduler, instance->ref, (uint64_t)payload));
}
void oreslang_source_actor_stop(void* opaque) {
    SourceActor* instance = (SourceActor*)opaque;
    ensure(instance && instance->magic == SOURCE_ACTOR_MAGIC);
    ensure(ores_native_actor_scheduler_drain(instance->scheduler));
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
