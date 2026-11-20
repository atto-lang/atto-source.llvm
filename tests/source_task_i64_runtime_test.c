#include "oreslang/runtime/SourceTaskRuntime.h"
#include "oreslang/runtime/NativeActorScheduler.h"

#include <assert.h>
#include <errno.h>
#include <time.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>

static _Atomic(uintptr_t) published_task = 0;
static _Atomic(int) carrier_await_status = -1;

/* Bounded guest-carrier observation: the producer must run before await. */
static pthread_mutex_t eager_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t eager_cond = PTHREAD_COND_INITIALIZER;
static int eager_started = 0;
static int64_t eager_producer(void) {
    assert(pthread_mutex_lock(&eager_mutex) == 0);
    eager_started = 1;
    assert(pthread_cond_signal(&eager_cond) == 0);
    assert(pthread_mutex_unlock(&eager_mutex) == 0);
    return 42;
}


typedef struct {
    void* task;
    _Atomic(int)* go;
    int status;
} DuplicateAwaitContext;

static int64_t simple_producer(void) {
    return 7;
}

static void* duplicate_await(void* opaque) {
    DuplicateAwaitContext* context = (DuplicateAwaitContext*)opaque;
    while (!atomic_load_explicit(context->go, memory_order_acquire))
        sched_yield();
    int64_t ignored = 0;
    context->status = oreslang_source_task_await_i64_checked(
        context->task, &ignored);
    return NULL;
}

static int64_t producer(void) {
    uintptr_t raw = 0;
    while ((raw = atomic_load_explicit(
                &published_task, memory_order_acquire)) == 0)
        sched_yield();

    int64_t impossible = 0;
    const int status = oreslang_source_task_await_i64_checked(
        (void*)raw, &impossible);
    atomic_store_explicit(
        &carrier_await_status, status, memory_order_release);
    return 41;
}

typedef struct {
    OresNativeActorScheduler* scheduler;
    OresNativeActorRef actor;
    _Atomic int invoked;
    _Atomic int status;
} ActorPoolFallbackProbe;

static void attempt_actor_task_on_native_carrier(
        void* raw, size_t actor_index, uint64_t payload) {
    ActorPoolFallbackProbe* probe = (ActorPoolFallbackProbe*)raw;
    assert(actor_index == 0 && payload == 1);
    assert(ores_native_actor_scheduler_in_any_actor_turn());
    assert(ores_native_actor_scheduler_is_current_actor(
        probe->scheduler, probe->actor));

    /* The scalar legacy ABI must not create its separate synthetic actor
     * scheduler when called from a genuine actor's admitted turn. */
    void* task = (void*)1;
    int status = oreslang_source_task_start_i64_checked(
        simple_producer, &task);
    assert(task == NULL);
    atomic_store_explicit(&probe->status, status, memory_order_release);
    atomic_store_explicit(&probe->invoked, 1, memory_order_release);
}

int main(void) {
    void* task = 0;
    assert(oreslang_source_task_start_i64_checked(producer, &task)
        == ORES_SOURCE_TASK_OK);
    assert(task != 0);
    atomic_store_explicit(
        &published_task, (uintptr_t)task, memory_order_release);

    int64_t result = 0;
    assert(oreslang_source_task_await_i64_checked(task, &result)
        == ORES_SOURCE_TASK_OK);
    assert(result == 41);
    assert(atomic_load_explicit(
        &carrier_await_status, memory_order_acquire)
        == ORES_SOURCE_TASK_WOULD_DEADLOCK);

    int64_t duplicate = 0;
    assert(oreslang_source_task_await_i64_checked(task, &duplicate)
        == ORES_SOURCE_TASK_ALREADY_CONSUMED);

    assert(oreslang_source_task_runtime_shutdown() == 1);
    assert(oreslang_source_task_runtime_shutdown() == 1);

    /*
     * Submit without immediately awaiting. A lazily scheduled Task deadlocks
     * this bounded condition-variable wait: no call to await can wake the
     * producer. Do not use sleep duration as the concurrency assertion.
     */
    void* eager_task = NULL;
    assert(oreslang_source_task_start_i64_checked(
        eager_producer, &eager_task) == ORES_SOURCE_TASK_OK);
    assert(eager_task != NULL);
    void* rejected_second = (void*)1;
    assert(oreslang_source_task_start_i64_checked(
        simple_producer, &rejected_second)
        == ORES_SOURCE_TASK_RESOURCE_EXHAUSTED);
    assert(rejected_second == NULL);

    struct timespec deadline;
    assert(timespec_get(&deadline, TIME_UTC) == TIME_UTC);
    deadline.tv_sec += 15;
    assert(pthread_mutex_lock(&eager_mutex) == 0);
    while (!eager_started) {
        const int waiting = pthread_cond_timedwait(
            &eager_cond, &eager_mutex, &deadline);
        assert(waiting == 0);
    }
    assert(pthread_mutex_unlock(&eager_mutex) == 0);
    int64_t eager_result = 0;
    assert(oreslang_source_task_await_i64_checked(
        eager_task, &eager_result) == ORES_SOURCE_TASK_OK);
    assert(eager_result == 42);
    assert(oreslang_source_task_runtime_shutdown() == 1);

    /*
     * Race duplicate post-consumption await admission against physical
     * shutdown/reclamation. The duplicate may enter before the closing fence
     * (ALREADY_CONSUMED) or after it (INVALID), but it must never touch freed
     * task synchronization state.
     */
    for (int i = 0; i < 64; ++i) {
        void* raced_task = NULL;
        assert(oreslang_source_task_start_i64_checked(
            simple_producer, &raced_task) == ORES_SOURCE_TASK_OK);
        int64_t raced_result = 0;
        assert(oreslang_source_task_await_i64_checked(
            raced_task, &raced_result) == ORES_SOURCE_TASK_OK);
        assert(raced_result == 7);

        _Atomic(int) go = 0;
        DuplicateAwaitContext context = {
            .task = raced_task,
            .go = &go,
            .status = -1
        };
        pthread_t duplicate;
        assert(pthread_create(
            &duplicate, NULL, duplicate_await, &context) == 0);
        atomic_store_explicit(&go, 1, memory_order_release);

        assert(oreslang_source_task_runtime_shutdown() == 1);
        assert(pthread_join(duplicate, NULL) == 0);
        assert(context.status == ORES_SOURCE_TASK_ALREADY_CONSUMED ||
            context.status == ORES_SOURCE_TASK_INVALID);
    }

    assert(oreslang_source_task_runtime_shutdown() == 1);

    /* A native actor callback is not permitted to escape to the singleton
     * legacy Task<int> worker pool. Outside the callback, no actor carrier
     * identity may leak onto this host thread. */
    assert(!ores_native_actor_scheduler_in_any_actor_turn());
    ActorPoolFallbackProbe probe = {0};
    OresNativeActorScheduler* actor_scheduler =
        ores_native_actor_scheduler_open(
            1, 4, 1, attempt_actor_task_on_native_carrier, &probe);
    assert(actor_scheduler != NULL);
    probe.scheduler = actor_scheduler;
    probe.actor = ores_native_actor_scheduler_spawn(actor_scheduler, 0);
    assert(probe.actor.scheduler_domain != 0);
    assert(ores_native_actor_scheduler_try_send(
        actor_scheduler, probe.actor, 1) == ORES_NATIVE_SEND_ACCEPTED);
    assert(ores_native_actor_scheduler_drain(actor_scheduler));
    assert(atomic_load_explicit(&probe.invoked, memory_order_acquire) == 1);
    assert(atomic_load_explicit(&probe.status, memory_order_acquire)
        == ORES_SOURCE_TASK_ACTOR_POOL_UNSUPPORTED);
    assert(ores_native_actor_scheduler_stop(actor_scheduler, probe.actor));
    ores_native_actor_scheduler_retire(actor_scheduler);
    assert(ores_native_actor_scheduler_try_destroy(actor_scheduler));
    assert(!ores_native_actor_scheduler_in_any_actor_turn());
    assert(oreslang_source_task_runtime_shutdown() == 1);
    return 0;
}
