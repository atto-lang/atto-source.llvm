#define _POSIX_C_SOURCE 200809L
#include "oreslang/runtime/NativeActorScheduler.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

/* Maximum admitted actor slots, four disjoint producers, real 8-worker pool.
 * Verify CONTROL priority is preserved, FIFO DATA and single physical entry.
 * No shared guest callback state outside actor ownership without atomics. */
enum { ACTORS = 1024, PRODUCERS = 4, PER_ACTOR = 4, WORKERS = 8 };
typedef struct {
    OresNativeActorScheduler* scheduler;
    OresNativeActorRef refs[ACTORS];
    atomic_int active[ACTORS];
    atomic_int received[ACTORS];
    atomic_int controls[ACTORS];
    atomic_int errors;
    atomic_int data_total;
    atomic_int control_total;
} State;
typedef struct {
    State* state;
    size_t offset;
} Producer;
static void require(int condition) { if (!condition) abort(); }
static void enter(State* s, size_t actor) {
    if (actor >= ACTORS
        || !ores_native_actor_scheduler_is_current_carrier(s->scheduler)
        || !ores_native_actor_scheduler_is_current_actor(s->scheduler, s->refs[actor])
        || atomic_fetch_add(&s->active[actor], 1) != 0)
        atomic_fetch_add(&s->errors, 1);
}
static void leave(State* s, size_t actor) {
    if (atomic_fetch_sub(&s->active[actor], 1) != 1)
        atomic_fetch_add(&s->errors, 1);
}
static void on_data(void* ctx, size_t actor, uint64_t payload) {
    State* s = ctx;
    enter(s, actor);
    if (payload != (uint64_t)atomic_fetch_add(&s->received[actor], 1))
        atomic_fetch_add(&s->errors, 1);
    atomic_fetch_add(&s->data_total, 1);
    leave(s, actor);
}
static void on_control(void* ctx, size_t actor, uint64_t payload) {
    State* s = ctx;
    enter(s, actor);
    if (payload != (uint64_t)(actor + 1024u)
        || atomic_fetch_add(&s->controls[actor], 1) != 0)
        atomic_fetch_add(&s->errors, 1);
    atomic_fetch_add(&s->control_total, 1);
    leave(s, actor);
}
static void* produce(void* raw) {
    Producer* p = raw;
    State* s = p->state;
    for (size_t i = p->offset; i < ACTORS; i += PRODUCERS) {
        OresNativeActorRef ref = s->refs[i];
        if ((i & 3u) == 0u)
            require(ores_native_actor_scheduler_publish_control(
                s->scheduler, ref, (uint64_t)(i + 1024u)));
        for (uint64_t m = 0; m < PER_ACTOR; ++m)
            require(ores_native_actor_scheduler_try_send(s->scheduler, ref, m)
                    == ORES_NATIVE_SEND_ACCEPTED);
    }
    return NULL;
}
int main(void) {
    /* The uninitialized actor array remains on the host heap, not a carrier stack. */
    State* s = calloc(1, sizeof(*s));
    require(s != NULL);
    s->scheduler = ores_native_actor_scheduler_open_control(
        ACTORS, 8, WORKERS, on_data, on_control, s);
    require(s->scheduler != NULL);
    require(ores_native_actor_scheduler_prestart_carriers(s->scheduler));
    for (size_t i = 0; i < ACTORS; ++i) {
        s->refs[i] = ores_native_actor_scheduler_spawn(s->scheduler, i);
        require(s->refs[i].scheduler_domain != 0);
    }
    pthread_t producers[PRODUCERS];
    Producer args[PRODUCERS];
    for (size_t i = 0; i < PRODUCERS; ++i) {
        args[i] = (Producer){s, i};
        require(!pthread_create(&producers[i], NULL, produce, &args[i]));
    }
    for (size_t i = 0; i < PRODUCERS; ++i)
        require(!pthread_join(producers[i], NULL));
    require(ores_native_actor_scheduler_drain(s->scheduler));
    require(atomic_load(&s->errors) == 0);
    require(atomic_load(&s->data_total) == ACTORS * PER_ACTOR);
    require(atomic_load(&s->control_total) == ACTORS / 4);
    for (size_t i = 0; i < ACTORS; ++i) {
        require(atomic_load(&s->active[i]) == 0);
        require(atomic_load(&s->received[i]) == PER_ACTOR);
        require(atomic_load(&s->controls[i]) == (int)((i & 3u) == 0u));
        require(ores_native_actor_scheduler_stop(s->scheduler, s->refs[i]));
    }
    ores_native_actor_scheduler_retire(s->scheduler);
    require(ores_native_actor_scheduler_try_destroy(s->scheduler));
    free(s);
    puts("NATIVE_READY_INDEX_1024_ACTOR_STRESS_PASS");
    return 0;
}
