#pragma once
#include <pthread.h>
#include <stdlib.h>

/* Test-only reusable barrier for POSIX systems including Darwin, which has
 * pthreads but does not expose pthread_barrier_t. A generation counter and
 * predicate loop prevent spurious wakeups from starting a race early. */
typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    unsigned remaining, participants, generation;
} OresTestBarrier;
static int ores_test_barrier_init(OresTestBarrier* b, unsigned participants) {
    if (!participants) abort();
    int result = pthread_mutex_init(&b->mutex, NULL);
    if (result) return result;
    result = pthread_cond_init(&b->condition, NULL);
    if (result) { pthread_mutex_destroy(&b->mutex); return result; }
    b->remaining = b->participants = participants;
    b->generation = 0;
    return 0;
}
static void ores_test_barrier_wait(OresTestBarrier* b) {
    if (pthread_mutex_lock(&b->mutex)) abort();
    unsigned generation = b->generation;
    if (--b->remaining == 0) {
        b->remaining = b->participants;
        ++b->generation;
        if (pthread_cond_broadcast(&b->condition)) abort();
    } else {
        while (generation == b->generation)
            if (pthread_cond_wait(&b->condition, &b->mutex)) abort();
    }
    if (pthread_mutex_unlock(&b->mutex)) abort();
}
static int ores_test_barrier_destroy(OresTestBarrier* b) {
    int result = pthread_cond_destroy(&b->condition);
    if (result) return result;
    return pthread_mutex_destroy(&b->mutex);
}
