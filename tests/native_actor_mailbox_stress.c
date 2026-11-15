/* Concurrent bounded-mailbox and actor-owned arena test for the C runtime.
 * Real guest actor handlers are separately compiled from .atto in CI.
 * This C harness exercises the low-level host ABI under producer contention.
 */
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

enum { ACTORS = 8, PRODUCERS = 4, PER_PRODUCER = 400 };
static int32_t handles[ACTORS];
static atomic_int counters[ACTORS];
static atomic_int errors;
static atomic_int capacity_retries;

extern int32_t atto_actor_spawn(int32_t);
extern int32_t atto_actor_send(int32_t, int64_t);
extern void atto_actor_finish(void);
extern int64_t atto_actor_state_load(int32_t);
extern void atto_actor_state_store(int32_t, int64_t);
extern uint32_t atto_actor_arena_alloc(uint32_t);

void atto_actor_init(int32_t actor_type) {
    /* State is actor-private, so every instance starts its own sequence at 0. */
    atto_actor_state_store(0, 0);
    if (atto_actor_state_load(0) != 0) atomic_fetch_add(&errors, 1);
    uint32_t offset = atto_actor_arena_alloc(32);
    if (offset < 8192u || offset >= 65536u) atomic_fetch_add(&errors, 1);
    if (atto_actor_arena_alloc(65536u) != 0) atomic_fetch_add(&errors, 1);
    (void)actor_type;
}

void atto_actor_receive(int32_t actor_type, int64_t value, int64_t sequence) {
    int64_t previous = atto_actor_state_load(0);
    if (previous != sequence) atomic_fetch_add(&errors, 1);
    if (value != 1) atomic_fetch_add(&errors, 1);
    atto_actor_state_store(0, previous + 1);
    atomic_fetch_add(&counters[actor_type], 1);
    if (actor_type == 0) {
        struct timespec slow = {0, 100000};
        nanosleep(&slow, NULL);
    }
}

static void *producer(void *arg) {
    int tid = *(int *)arg;
    for (int i = 0; i < PER_PRODUCER; ++i) {
        int target = (i + tid) % ACTORS;
        int retries = 0;
        for (;;) {
            int result = atto_actor_send(handles[target], 1);
            if (result == 0) break;
            if (result != -2 || ++retries >= 1000000) {
                atomic_fetch_add(&errors, 1);
                return NULL;
            }
            atomic_fetch_add(&capacity_retries, 1);
            sched_yield();
        }
    }
    return NULL;
}

int main(void) {
    for (int i = 0; i < ACTORS; ++i) {
        handles[i] = atto_actor_spawn(i);
        assert(handles[i] > 0);
    }
    /* Forged generation and invalid slot cannot address actor memory. */
    assert(atto_actor_send(0, 1) == -1);
    assert(atto_actor_send(handles[0] ^ (1 << 16), 1) == -1);
    assert(atto_actor_send(0x7fffffff, 1) == -1);
    pthread_t threads[PRODUCERS];
    int ids[PRODUCERS];
    for (int t = 0; t < PRODUCERS; ++t) {
        ids[t] = t;
        assert(pthread_create(&threads[t], NULL, producer, &ids[t]) == 0);
    }
    for (int t = 0; t < PRODUCERS; ++t) {
        assert(pthread_join(threads[t], NULL) == 0);
    }
    /* The bounded native profile admits 32 actors, never a 33rd. */
    int32_t extra_handles[32 - ACTORS];
    for (int i = ACTORS; i < 32; ++i) {
        extra_handles[i - ACTORS] = atto_actor_spawn(i % ACTORS);
        assert(extra_handles[i - ACTORS] > 0);
    }
    assert(atto_actor_spawn(0) == 0);
    /* Close admission and drain all accepted messages before destroying
     * any actor-private arena or stack. No fake sleeps for completion. */
    atto_actor_finish();
    for (int i = 0; i < 32 - ACTORS; ++i) {
        assert(atto_actor_send(extra_handles[i], 1) == -1);
    }
    for (int t = 0; t < ACTORS; ++t) {
        int got = atomic_load(&counters[t]);
        if (got != (PRODUCERS * PER_PRODUCER / ACTORS)) {
            fprintf(stderr, "actor %d delivered %d\n", t, got);
            return 1;
        }
        assert(atto_actor_send(handles[t], 1) == -1);
    }
    assert(atto_actor_spawn(0) == 0);
    assert(atomic_load(&errors) == 0);
    printf("PASS C actor mailbox concurrency 1600 messages, 8 private arenas, close/drain, stale refs, retries=%d\n",
           atomic_load(&capacity_retries));
    return 0;
}
