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
#include <time.h>

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

/* The original stress test only checked an upper bound. This bounded
 * handshake positively proves that a gate with two permits actually admits
 * two physical threads simultaneously while denying a third entrant. */
typedef struct {
    OresNativePhysicalTurnGate gate;
    _Atomic int entered, release, denied;
} TwoCarrierGate;

static void* fixed_carrier(void* raw) {
    TwoCarrierGate* state = (TwoCarrierGate*)raw;
    if (!ores_native_turn_gate_try_acquire(&state->gate)) {
        atomic_fetch_add(&state->denied, 1);
        return NULL;
    }
    atomic_fetch_add(&state->entered, 1);
    while (!atomic_load(&state->release)) sched_yield();
    ores_native_turn_gate_release(&state->gate);
    return NULL;
}

static void prove_exactly_two_physical_carriers(void) {
    TwoCarrierGate state = {0};
    state.gate.maximum = 2;
    pthread_t carriers[2];
    assert(pthread_create(&carriers[0], NULL, fixed_carrier, &state) == 0);
    assert(pthread_create(&carriers[1], NULL, fixed_carrier, &state) == 0);
    int overlap = 0;
    for (int i = 0; i < 5000; i++) {
        if (atomic_load(&state.entered) == 2) { overlap = 1; break; }
        struct timespec pause = {0, 1000000};
        nanosleep(&pause, NULL);
    }
    if (overlap) {
        assert(ores_native_turn_gate_active(&state.gate) == 2);
        assert(!ores_native_turn_gate_try_acquire(&state.gate));
    }
    /* Always release first, even on a scheduling timeout. */
    atomic_store(&state.release, 1);
    assert(pthread_join(carriers[0], NULL) == 0);
    assert(pthread_join(carriers[1], NULL) == 0);
    assert(overlap);
    assert(atomic_load(&state.denied) == 0);
    assert(ores_native_turn_gate_active(&state.gate) == 0);
    assert(ores_native_turn_gate_try_acquire(&state.gate));
    ores_native_turn_gate_release(&state.gate);
    assert(ores_native_turn_gate_active(&state.gate) == 0);
}

static void prove_waiter_notification_contract(void) {
    State state = {0};
    state.gate.maximum = 1;
    state.gate.context = &state;
    state.gate.notify_free = notify;
    assert(ores_native_turn_gate_try_acquire(&state.gate));
    assert(!ores_native_turn_gate_try_acquire(&state.gate));
    ores_native_turn_gate_release(&state.gate);
    assert(atomic_load(&state.notifications) == 0);
    assert(ores_native_turn_gate_try_acquire(&state.gate));
    atomic_store(&state.gate.waiting, 1);
    ores_native_turn_gate_release(&state.gate);
    assert(atomic_load(&state.notifications) == 1);
    atomic_store(&state.gate.waiting, 0);
    assert(ores_native_turn_gate_active(&state.gate) == 0);
}

int main(void) {
    run_with_limit(1);
    run_with_limit(4);
    prove_exactly_two_physical_carriers();
    prove_waiter_notification_contract();
    puts("native shared gate multi-thread bounded admission: OK physical_overlap=2 notify_waiter=1");
    return 0;
}
