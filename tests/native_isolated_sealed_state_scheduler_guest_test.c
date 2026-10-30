/* Real compiler-emitted isolated actor handler, executed on native scheduler
 * DATA leases. No guest handler reimplementation or early-stop zero-work pass.
 * This is synchronous only: retained DATA + WAKE stay a separate gate. */
#define _POSIX_C_SOURCE 200809L
#include "oreslang/runtime/NativeActorScheduler.h"
#include "oreslang/runtime/ActorOwnedI64Heap.h"
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

extern void oreslang_actor_Counter_init(OresActorI64Heap*, uint64_t, uint64_t);
extern int64_t oreslang_actor_Counter_on_message(
    OresActorI64Heap*, uint64_t, uint64_t, int64_t);

#define VERIFY(expr) do { if (!(expr)) { \
    fprintf(stderr, "SEALED_SCHEDULER_FAIL %s:%d %s\n", \
            __FILE__, __LINE__, #expr); abort(); \
} } while (0)

typedef struct {
    OresNativeActorScheduler* scheduler;
    OresNativeActorRef actor;
    OresActorI64Heap* heap;
    int initialized; /* accessed only by the scheduler-owned actor DATA lane */
    int writing;
    atomic_int processed;
    atomic_int_fast64_t observed_last;
} Fixture;

static void on_data(void* raw, size_t actor_index, uint64_t payload) {
    Fixture* f = (Fixture*)raw;
    VERIFY(actor_index == 0);
    VERIFY(ores_native_actor_scheduler_is_current_actor(f->scheduler, f->actor));
    VERIFY(payload == 1 || payload == 2);
    OresActorTurn turn = ores_actor_i64_heap_enter(
        f->heap, 912, f->actor.generation);
    if (!f->initialized) {
        oreslang_actor_Counter_init(f->heap, turn.domain, turn.epoch);
        f->initialized = 1;
    }
    const int previous = atomic_load_explicit(
        &f->processed, memory_order_relaxed);
    VERIFY((previous == 0 && payload == 1) ||
           (previous == 1 && payload == 2));
    const int64_t result = oreslang_actor_Counter_on_message(
        f->heap, turn.domain, turn.epoch, (int64_t)payload);
    VERIFY(result == (f->writing ? (int64_t)(previous == 0 ? 1 : 3)
                                : (int64_t)(previous == 0 ? 8 : 9)));
    ores_actor_i64_heap_leave(f->heap, turn);
    atomic_store_explicit(&f->observed_last, result, memory_order_release);
    atomic_store_explicit(&f->processed, previous + 1, memory_order_release);
}

int main(int argc, char** argv) {
    VERIFY(argc == 2);
    const int writing = argv[1][0] == 'w' && argv[1][1] == '\0';
    const int reading = argv[1][0] == 'r' && argv[1][1] == '\0';
    VERIFY(writing || reading);
    Fixture f = {0};
    f.writing = writing;
    f.scheduler = ores_native_actor_scheduler_open(1, 2, 2, on_data, &f);
    VERIFY(f.scheduler);
    f.actor = ores_native_actor_scheduler_spawn(f.scheduler, 0);
    VERIFY(f.actor.scheduler_domain && f.actor.generation);
    f.heap = ores_actor_i64_heap_open(912, f.actor.generation, 1);
    VERIFY(f.heap);
    VERIFY(ores_native_actor_scheduler_send(f.scheduler, f.actor, 1));
    VERIFY(ores_native_actor_scheduler_send(f.scheduler, f.actor, 2));

    /* Wait for actual guest callbacks, not merely a scheduler completion or
     * a successful STOP. No unbounded CI waits; fail loudly if stalled. */
    for (unsigned iteration = 0; iteration < 8000; ++iteration) {
        if (atomic_load_explicit(&f.processed, memory_order_acquire) == 2)
            break;
        const struct timespec sleep_for = {0, 1000000};
        VERIFY(nanosleep(&sleep_for, NULL) == 0);
    }
    VERIFY(atomic_load_explicit(&f.processed, memory_order_acquire) == 2);
    VERIFY(atomic_load_explicit(&f.observed_last, memory_order_acquire)
           == (writing ? 3 : 9));
    VERIFY(ores_native_actor_scheduler_drain(f.scheduler));
    VERIFY(ores_native_actor_scheduler_accepted(f.scheduler) == 2);
    VERIFY(ores_native_actor_scheduler_completed(f.scheduler) == 2);
    VERIFY(ores_native_actor_scheduler_stop(f.scheduler, f.actor));
    VERIFY(ores_native_actor_scheduler_wait_done(f.scheduler, f.actor));
    OresNativeActorTerminalState terminal = {0};
    VERIFY(ores_native_actor_scheduler_terminal_state(
        f.scheduler, f.actor, &terminal));
    VERIFY(terminal.kind == ORES_NATIVE_TERMINATION_STOPPED);
    VERIFY(terminal.accepted_user_messages == 2);
    VERIFY(terminal.processed_user_messages == 2);
    VERIFY(terminal.abandoned_user_messages == 0);

    /* Only a physical terminal ACK authorizes heap retirement. */
    ores_actor_i64_heap_retire(f.heap);
    VERIFY(ores_actor_i64_heap_try_destroy(f.heap));
    f.heap = NULL;
    ores_native_actor_scheduler_retire(f.scheduler);
    VERIFY(ores_native_actor_scheduler_try_destroy(f.scheduler));
    printf("COMPILED_SEALED_SCHEDULER_%s_PASS processed=2 abandoned=0\n",
           writing ? "WRITE" : "READ");
    return 0;
}
