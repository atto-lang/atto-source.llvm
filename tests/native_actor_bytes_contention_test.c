#define _POSIX_C_SOURCE 200809L
#include "oreslang/runtime/NativeActorBytesInbox.h"
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PRODUCERS 4
#define PER_PRODUCER 256
#define TOTAL (PRODUCERS * PER_PRODUCER)

typedef struct {
    OresNativeActorBytesInbox* box;
    OresNativeActorBytesRef ref;
    _Atomic unsigned done;
    _Atomic unsigned busy;
    _Atomic unsigned full;
    _Atomic unsigned received;
} Session;
typedef struct { Session* session; unsigned producer; } Producer;
static void check(int good) { if (!good) abort(); }
static void* produce(void* arg) {
    Producer* p = (Producer*)arg;
    Session* session = p->session;
    for (unsigned n = 0; n < PER_PRODUCER; ++n) {
        unsigned char frame[8] = {(unsigned char)p->producer,
            (unsigned char)(n & 255u), (unsigned char)(n >> 8), 0, 255, 10, 0, 128};
        for (;;) {
            int r = ores_native_actor_bytes_try_send(
                session->box, session->ref, frame, sizeof(frame));
            if (r == ORES_ACTOR_BYTES_OK) break;
            if (r == ORES_ACTOR_BYTES_BUSY)
                atomic_fetch_add_explicit(&session->busy, 1, memory_order_relaxed);
            else if (r == ORES_ACTOR_BYTES_DENIED)
                atomic_fetch_add_explicit(&session->full, 1, memory_order_relaxed);
            else abort();
            /* HOST TEST ONLY. A real guest actor must suspend/requeue, never
             * park or spin to retry while carrying the actor DATA turn. */
            sched_yield();
        }
    }
    atomic_fetch_add_explicit(&session->done, 1, memory_order_release);
    return NULL;
}
int main(void) {
    Session session = {0};
    session.box = ores_native_actor_bytes_open(455, 19, 128, 32, 1024);
    check(session.box != NULL);
    session.ref = ores_native_actor_bytes_ref(session.box);
    pthread_t threads[PRODUCERS];
    Producer args[PRODUCERS];
    for (unsigned i=0;i<PRODUCERS;++i) {
        args[i]=(Producer){&session,i};
        check(pthread_create(&threads[i],NULL,produce,&args[i])==0);
    }
    unsigned expected[PRODUCERS] = {0};
    while (atomic_load_explicit(&session.received, memory_order_relaxed) < TOTAL) {
        unsigned char out[8] = {0}; size_t n = 0;
        int r = ores_native_actor_bytes_try_receive(
            session.box, session.ref, out, sizeof(out), &n);
        if (r == ORES_ACTOR_BYTES_OK) {
            check(n == 8 && out[0] < PRODUCERS && out[3] == 0 &&
                  out[4] == 255 && out[5] == 10 && out[6] == 0 && out[7] == 128);
            unsigned seq = (unsigned)out[1] | ((unsigned)out[2] << 8);
            check(seq == expected[out[0]] && seq < PER_PRODUCER);
            expected[out[0]]++;
            atomic_fetch_add_explicit(&session.received, 1, memory_order_relaxed);
        } else {
            check(r == ORES_ACTOR_BYTES_DENIED || r == ORES_ACTOR_BYTES_BUSY);
            sched_yield();
        }
    }
    for (unsigned i=0;i<PRODUCERS;++i) {
        check(pthread_join(threads[i],NULL)==0);
        check(expected[i]==PER_PRODUCER);
    }
    check(atomic_load_explicit(&session.done,memory_order_acquire)==PRODUCERS);
    size_t pending=123, used=123;
    check(ores_native_actor_bytes_observe(session.box,session.ref,&pending,&used)
        == ORES_ACTOR_BYTES_OK);
    check(pending==0 && used==0);
    check(ores_native_actor_bytes_all_zero_for_test(session.box));
    check(ores_native_actor_bytes_try_retire(session.box,session.ref)
        == ORES_ACTOR_BYTES_OK);
    check(ores_native_actor_bytes_try_destroy(session.box,session.ref)
        == ORES_ACTOR_BYTES_OK);
    printf("NATIVE_ACTOR_BYTES_CONTENTION_HOST_ONLY_PASS producers=%u records=%u per_producer_fifo=1 once=1 zeroized=1 busy=%u full=%u\n",
        PRODUCERS, TOTAL, atomic_load(&session.busy), atomic_load(&session.full));
    return 0;
}
