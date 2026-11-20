#define _POSIX_C_SOURCE 200809L
#include "oreslang/runtime/NativeActorScheduler.h"
#if defined(NDEBUG)
#undef NDEBUG // Tests must not silently disappear in optimized CMake builds
#endif
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

/* Carrier affinity (CONTINUATION_LOCALITY_V1 rule 5) for the native scheduler.
 *
 * Safety, in every mode: each message runs exactly once, per actor in FIFO
 * order, and never concurrently with another turn of the same actor.
 * Locality under backlog (8 actors x 50 queued turns on 4 carriers): the
 * window lets a carrier take the actor it ran last ahead of the queue head.
 * Measured on an M-series Mac: 0.99 of turns stay on the same carrier with the
 * window, 0.96 without it (the existing two-consecutive warm-cache rule
 * already provides most of it). The test asserts that the window is used and
 * never makes locality worse; it does not claim a fixed speedup. With the
 * window at 0 the queue is plain FIFO and affine_picks stays 0.
 *
 * Not covered by the window: when carriers are idle, the carrier that runs a
 * newly ready actor is whichever one the condition variable wakes. Targeted
 * wakeup of the last carrier is separate work. */

enum { ACTORS = 8, WORKERS = 4, ROUNDS = 40, PER_ROUND = 50 };

typedef struct {
    pthread_mutex_t mutex;
    pthread_t last[ACTORS];
    int has_last[ACTORS];
    uint64_t next_payload[ACTORS];
    atomic_int in_turn[ACTORS];
    long same_carrier, turns, violations;
} State;

static void on_message(void* ctx, size_t actor, uint64_t payload) {
    State* s = (State*)ctx;
    if (atomic_fetch_add(&s->in_turn[actor], 1) != 0) {
        assert(pthread_mutex_lock(&s->mutex) == 0);
        ++s->violations; /* two turns of one actor overlapped */
        assert(pthread_mutex_unlock(&s->mutex) == 0);
    }
    assert(pthread_mutex_lock(&s->mutex) == 0);
    if (payload != s->next_payload[actor]) ++s->violations; /* FIFO or exactly-once broken */
    s->next_payload[actor] = payload + 1;
    pthread_t self = pthread_self();
    if (s->has_last[actor] && pthread_equal(s->last[actor], self)) ++s->same_carrier;
    s->last[actor] = self;
    s->has_last[actor] = 1;
    ++s->turns;
    assert(pthread_mutex_unlock(&s->mutex) == 0);
    struct timespec start, now;
    clock_gettime(CLOCK_MONOTONIC, &start);
    do clock_gettime(CLOCK_MONOTONIC, &now);
    while ((now.tv_sec - start.tv_sec) * 1000000000L + (now.tv_nsec - start.tv_nsec) < 20000L);
    atomic_fetch_sub(&s->in_turn[actor], 1);
}

static void on_control(void* ctx, size_t actor, uint64_t payload) {
    (void)ctx; (void)actor; (void)payload;
}

static double run(uint32_t window, uint64_t* affine_picks) {
    State state;
    memset(&state, 0, sizeof state);
    assert(pthread_mutex_init(&state.mutex, NULL) == 0);
    OresNativeActorScheduler* scheduler = ores_native_actor_scheduler_open_control(
            ACTORS, PER_ROUND, WORKERS, on_message, on_control, &state);
    assert(scheduler != NULL);
    assert(ores_native_actor_scheduler_set_affinity_window(scheduler, window));
    assert(!ores_native_actor_scheduler_set_affinity_window(scheduler, 65));
    OresNativeActorRef refs[ACTORS];
    for (size_t a = 0; a < ACTORS; ++a) {
        refs[a] = ores_native_actor_scheduler_spawn(scheduler, a);
        assert(refs[a].scheduler_domain);
    }
    for (uint64_t round = 0; round < ROUNDS; ++round) {
        for (uint64_t k = 0; k < PER_ROUND; ++k)
            for (size_t a = 0; a < ACTORS; ++a)
                assert(ores_native_actor_scheduler_send(scheduler, refs[a], round * PER_ROUND + k));
        assert(ores_native_actor_scheduler_drain(scheduler));
    }
    *affine_picks = ores_native_actor_scheduler_affine_picks(scheduler);
    ores_native_actor_scheduler_retire(scheduler);
    assert(ores_native_actor_scheduler_try_destroy(scheduler));

    assert(state.violations == 0);
    assert(state.turns == (long)ACTORS * ROUNDS * PER_ROUND);
    for (size_t a = 0; a < ACTORS; ++a) assert(state.next_payload[a] == (uint64_t)ROUNDS * PER_ROUND);
    assert(pthread_mutex_destroy(&state.mutex) == 0);
    return (double)state.same_carrier / (double)(state.turns - ACTORS);
}

int main(void) {
    uint64_t picks_on = 0, picks_off = 0;
    const double on = run(8, &picks_on);
    const double off = run(0, &picks_off);
    printf("same-carrier ratio: window 8 = %.2f (affine picks %llu), window 0 = %.2f\n",
            on, (unsigned long long)picks_on, off);
    assert(picks_off == 0); /* window 0 is plain FIFO */
    assert(picks_on > 0);           /* the window is actually used */
    assert(on + 0.02 >= off);       /* and never makes locality worse than FIFO */
    puts("native carrier affinity: exactly-once FIFO exclusive turns, locality: OK");
    return 0;
}
