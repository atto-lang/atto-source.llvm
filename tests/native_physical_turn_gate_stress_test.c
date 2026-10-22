#define _POSIX_C_SOURCE 200809L
#include "oreslang/runtime/NativePhysicalTurnGate.h"
#if defined(NDEBUG)
#undef NDEBUG // Tests must not silently disappear in optimized CMake builds
#endif
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>

enum { THREADS = 8, LOOPS = 3000 };
typedef struct {
    OresNativePhysicalTurnGate gate;
    _Atomic int live, peak, completed, notifications;
} State;

static void notify(void* ptr) {
    State* state = (State*)ptr;
    atomic_fetch_add(&state->notifications, 1);
}

static void* worker(void* ptr) {
    State* state = (State*)ptr;
    for (int i = 0; i < LOOPS; ++i) {
        for (;;) {
            if (ores_native_turn_gate_try_acquire(&state->gate)) break;
            atomic_fetch_add(&state->gate.waiting, 1);
            if (ores_native_turn_gate_try_acquire(&state->gate)) {
                atomic_fetch_sub(&state->gate.waiting, 1);
                break;
            }
            sched_yield();
            atomic_fetch_sub(&state->gate.waiting, 1);
        }
        int count = atomic_fetch_add(&state->live, 1) + 1;
        int high = atomic_load(&state->peak);
        while (count > high
                && !atomic_compare_exchange_weak(&state->peak, &high, count)) { }
        if (i % 5 == 0) sched_yield();
        atomic_fetch_sub(&state->live, 1);
        ores_native_turn_gate_release(&state->gate);
        atomic_fetch_add(&state->completed, 1);
    }
    return NULL;
}

static void run_with_limit(size_t maximum) {
    State state = {0};
    state.gate.maximum = maximum;
    state.gate.context = &state;
    state.gate.notify_free = notify;
    pthread_t workers[THREADS];
    for (int i = 0; i < THREADS; ++i)
        assert(pthread_create(&workers[i], NULL, worker, &state) == 0);
    for (int i = 0; i < THREADS; ++i)
        assert(pthread_join(workers[i], NULL) == 0);
    assert(atomic_load(&state.live) == 0);
    assert(ores_native_turn_gate_active(&state.gate) == 0);
    assert(atomic_load(&state.completed) == THREADS * LOOPS);
    assert(atomic_load(&state.peak) >= 1);
    assert((size_t)atomic_load(&state.peak) <= maximum);
}

int main(void) {
    run_with_limit(1);
    run_with_limit(4);
    puts("native shared gate multi-thread bounded admission: OK");
    return 0;
}
