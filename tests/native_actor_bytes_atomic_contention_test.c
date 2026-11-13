#define _POSIX_C_SOURCE 200809L
/* Trusted native host test, NOT compiled .atto source or a broker.
 * Only external test producers yield; an actor turn never waits/spins. */
#include "oreslang/runtime/NativeActorBytesSchedulerBinding.h"
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum { PRODUCERS = 4, PER_PRODUCER = 256, TOTAL = PRODUCERS * PER_PRODUCER };
typedef struct {
    OresNativeActorScheduler* scheduler;
    OresNativeActorBytesSchedulerBinding binding;
    atomic_uint processed;
    atomic_uint producers_done;
    atomic_uint retries;
    unsigned expected[PRODUCERS]; /* accessed ONLY on single-flight DATA lane */
} Session;
typedef struct { Session* session; unsigned producer; } Producer;

#define check(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "NATIVE_BYTES_ATOMIC_CONTENTION_FAIL line=%d expr=%s\n", \
                __LINE__, #condition); \
        abort(); \
    } \
} while (0)
static void on_data(void* opaque, size_t index, uint64_t token) {
    Session* s = (Session*)opaque;
    check(index == 0);
    unsigned p = (unsigned)(token >> 32);
    unsigned n = (unsigned)(token & UINT64_C(0xffffffff));
    check(p < PRODUCERS && n < PER_PRODUCER);
    unsigned char bytes[8] = {0};
    size_t length = SIZE_MAX;
    /* This path authenticates the physical actor's current DATA ordinal
     * and consumes only the matching, scheduler-committed private slab. */
    check(ores_native_actor_bytes_receive_current_data(
        &s->binding, bytes, sizeof(bytes), &length) == ORES_ACTOR_BYTES_OK);
    check(length == sizeof(bytes) && bytes[0] == (unsigned char)p &&
        bytes[1] == (unsigned char)n && bytes[2] == (unsigned char)(n >> 8) &&
        bytes[3] == 0 && bytes[4] == 255 && bytes[5] == 10 &&
        bytes[6] == 13 && bytes[7] == 128);
    check(s->expected[p] == n); /* each producer's FIFO is preserved */
    ++s->expected[p];
    atomic_fetch_add_explicit(&s->processed, 1u, memory_order_release);
}
static void* producer_main(void* opaque) {
    Producer* source = (Producer*)opaque;
    Session* s = source->session;
    for (unsigned n = 0; n < PER_PRODUCER; ++n) {
        unsigned char frame[8] = {
            (unsigned char)source->producer, (unsigned char)n,
            (unsigned char)(n >> 8), 0, 255, 10, 13, 128
        };
        const uint64_t token = ((uint64_t)source->producer << 32) | n;
        unsigned attempts = 0;
        for (;;) {
            OresNativeSendStatus result =
                ores_native_actor_bytes_try_send_atomic(
                    &s->binding, token, frame, sizeof(frame));
            if (result == ORES_NATIVE_SEND_ACCEPTED) break;
            check(result == ORES_NATIVE_SEND_MAILBOX_FULL ||
                  result == ORES_NATIVE_SEND_RESOURCE_EXHAUSTED);
            check(++attempts < 10000000u);
            atomic_fetch_add_explicit(&s->retries, 1u, memory_order_relaxed);
            sched_yield(); /* HOST test producer, never the actor worker */
        }
    }
    atomic_fetch_add_explicit(&s->producers_done, 1u, memory_order_release);
    return NULL;
}
int main(void) {
    Session session = {0};
    session.scheduler = ores_native_actor_scheduler_open(
        1, 16, 1, on_data, &session);
    check(session.scheduler != NULL);
    OresNativeActorRef actor =
        ores_native_actor_scheduler_spawn(session.scheduler, 0);
    check(actor.scheduler_domain != 0 && actor.generation != 0);
    OresNativeActorBytesInbox* inbox =
        ores_native_actor_bytes_open(1, actor.generation, 16, 8, 128);
    check(inbox != NULL);
    check(ores_native_actor_bytes_bind_scheduler(
        &session.binding, session.scheduler, actor, inbox));
    check(ores_native_actor_scheduler_try_send(session.scheduler, actor, 9)
          == ORES_NATIVE_SEND_WRONG_MAIL_KIND);
    check(!ores_native_actor_scheduler_submit(session.scheduler, 0, 9));

    pthread_t threads[PRODUCERS];
    Producer producers[PRODUCERS];
    for (unsigned i = 0; i < PRODUCERS; ++i) {
        producers[i] = (Producer){&session, i};
        check(pthread_create(&threads[i], NULL, producer_main,
                             &producers[i]) == 0);
    }
    for (unsigned i = 0; i < PRODUCERS; ++i)
        check(pthread_join(threads[i], NULL) == 0);
    check(atomic_load_explicit(&session.producers_done, memory_order_acquire)
          == PRODUCERS);
    const struct timespec one_ms = {0, 1000000};
    for (unsigned i = 0; i < 15000; ++i) {
        if (atomic_load_explicit(&session.processed, memory_order_acquire)
            == TOTAL) break;
        nanosleep(&one_ms, NULL); /* external test control thread only */
    }
    check(atomic_load_explicit(&session.processed, memory_order_acquire)
          == TOTAL);
    for (unsigned i = 0; i < PRODUCERS; ++i)
        check(session.expected[i] == PER_PRODUCER);
    check(ores_native_actor_scheduler_stop_sealed(session.scheduler, actor));
    check(ores_native_actor_scheduler_wait_done(session.scheduler, actor));
    OresNativeActorTerminalState terminal = {0};
    check(ores_native_actor_scheduler_terminal_state(
        session.scheduler, actor, &terminal));
    check(terminal.accepted_user_messages == TOTAL &&
          terminal.processed_user_messages == TOTAL &&
          terminal.abandoned_user_messages == 0);
    size_t queued = SIZE_MAX, bytes_used = SIZE_MAX;
    check(ores_native_actor_bytes_observe(
        inbox, session.binding.storage, &queued, &bytes_used)
          == ORES_ACTOR_BYTES_OK);
    check(queued == 0 && bytes_used == 0);
    check(ores_native_actor_bytes_try_retire(
        inbox, session.binding.storage) == ORES_ACTOR_BYTES_OK);
    check(ores_native_actor_bytes_try_destroy(
        inbox, session.binding.storage) == ORES_ACTOR_BYTES_OK);
    ores_native_actor_scheduler_retire(session.scheduler);
    check(ores_native_actor_scheduler_drain(session.scheduler));
    check(ores_native_actor_scheduler_try_destroy(session.scheduler));
    printf("NATIVE_ACTOR_BYTES_ATOMIC_CONTENTION_HOST_ONLY_PASS producers=%u messages=%u exact_once=1 per_producer_fifo=1 terminal_accounting=1 slab_reclaimed=1 retries=%u\n",
           PRODUCERS, TOTAL, atomic_load(&session.retries));
    return 0;
}
