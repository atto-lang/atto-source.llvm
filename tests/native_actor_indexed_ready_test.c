#define _POSIX_C_SOURCE 200809L
#include "oreslang/runtime/NativeActorScheduler.h"
#include <assert.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sched.h>
#include <time.h>

enum { ACTORS = 4096, ROUNDS = 4 };
typedef struct {
    atomic_int* next;
    atomic_int* active;
    atomic_int errors, controls, total;
} State;
static void enter(State* s, size_t actor) {
    if (atomic_fetch_add(&s->active[actor], 1) != 0)
        atomic_fetch_add(&s->errors, 1);
}
static void leave(State* s, size_t actor) {
    if (atomic_fetch_sub(&s->active[actor], 1) != 1)
        atomic_fetch_add(&s->errors, 1);
}
static void data(void* raw, size_t actor, uint64_t payload) {
    State* s = (State*)raw;
    enter(s, actor);
    if (payload != (uint64_t)atomic_fetch_add(&s->next[actor], 1))
        atomic_fetch_add(&s->errors, 1);
    leave(s, actor);
    atomic_fetch_add(&s->total, 1);
}
static void control(void* raw, size_t actor, uint64_t payload) {
    State* s = (State*)raw;
    enter(s, actor);
    if (payload != UINT64_C(0xdecafbad))
        atomic_fetch_add(&s->errors, 1);
    leave(s, actor);
    atomic_fetch_add(&s->controls, 1);
}

// The first DATA callback holds the sole carrier until the test has queued
// lower-slot DATA and high-slot CONTROL. This proves dispatch priority without
// relying on incidental pthread scheduling or elapsed-time comparisons.
typedef struct {
    atomic_int entered, release, dispatched, control_position, seen, errors;
    atomic_uint_fast64_t seen_data_slots;
} PriorityState;
static void priority_data(void* raw, size_t actor, uint64_t payload) {
    PriorityState* p = (PriorityState*)raw;
    if (actor == 0) {
        atomic_store_explicit(&p->entered, 1, memory_order_release);
        while (!atomic_load_explicit(&p->release, memory_order_acquire))
            sched_yield();
    }
    if (payload != actor) atomic_fetch_add(&p->errors, 1);
    /* Bit-address the six selected actor IDs, not the delivery count: a
     * duplicated valid DATA turn may otherwise conceal a missing one. */
    const size_t slots[] = {0, 63, 64, 4095, 4096, 65535};
    unsigned index = 0;
    for (; index < sizeof(slots) / sizeof(slots[0]); ++index)
        if (slots[index] == actor) break;
    if (index == sizeof(slots) / sizeof(slots[0])) {
        atomic_fetch_add(&p->errors, 1);
    } else {
        uint_fast64_t bit = UINT64_C(1) << index;
        if (atomic_fetch_or(&p->seen_data_slots, bit) & bit)
            atomic_fetch_add(&p->errors, 1);
    }
    atomic_fetch_add(&p->dispatched, 1);
    atomic_fetch_add(&p->seen, 1);
}
static void priority_control(void* raw, size_t actor, uint64_t payload) {
    PriorityState* p = (PriorityState*)raw;
    if (actor != 65535 || payload != UINT64_C(0xdecafbad))
        atomic_fetch_add(&p->errors, 1);
    int position = atomic_fetch_add(&p->dispatched, 1);
    atomic_store_explicit(&p->control_position, position, memory_order_release);
}
static void run_priority_boundary_regression(void) {
    const size_t indices[] = {0, 63, 64, 4095, 4096, 65535};
    enum { COUNT = sizeof(indices) / sizeof(indices[0]) };
    PriorityState state = {0};
    atomic_init(&state.control_position, -1);
    OresNativeActorScheduler* scheduler = ores_native_actor_scheduler_open_control(
        65536, 1, 1, priority_data, priority_control, &state);
    assert(scheduler);
    OresNativeActorRef refs[COUNT];
    for (size_t i = 0; i < COUNT; ++i) {
        refs[i] = ores_native_actor_scheduler_spawn(scheduler, indices[i]);
        assert(refs[i].scheduler_domain);
    }
    assert(ores_native_actor_scheduler_send(scheduler, refs[0], indices[0]));
    struct timespec start, now;
    assert(clock_gettime(CLOCK_MONOTONIC, &start) == 0);
    while (!atomic_load_explicit(&state.entered, memory_order_acquire)) {
        assert(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
        assert(now.tv_sec - start.tv_sec < 10); /* no unbounded spin */
        sched_yield();
    }
    for (size_t i = 1; i < COUNT; ++i)
        assert(ores_native_actor_scheduler_send(scheduler, refs[i], indices[i]));
    assert(ores_native_actor_scheduler_publish_control(
        scheduler, refs[COUNT - 1], UINT64_C(0xdecafbad)));
    /* Stop wins against NEW sends, not against work already admitted before
     * the stop linearized. The stopped actor must drain its high-slot
     * CONTROL first and then its previously accepted DATA, with no ABA. */
    assert(ores_native_actor_scheduler_stop(scheduler, refs[COUNT - 1]));
    assert(!ores_native_actor_scheduler_poll_ready(
        scheduler, refs[COUNT - 1]));
    assert(ores_native_actor_scheduler_try_send(
        scheduler, refs[COUNT - 1], UINT64_C(12345))
        == ORES_NATIVE_SEND_TERMINATED);
    assert(!ores_native_actor_scheduler_publish_control(
        scheduler, refs[COUNT - 1], UINT64_C(0xdecafbad)));
    assert(!ores_native_actor_scheduler_poll_done(
        scheduler, refs[COUNT - 1]));
    atomic_store_explicit(&state.release, 1, memory_order_release);
    assert(ores_native_actor_scheduler_drain(scheduler));
    /* The blocker is dispatched first, then globally earliest CONTROL,
     * even though several lower-index DATA bits are already ready. */
    assert(atomic_load(&state.control_position) == 1);
    assert(atomic_load(&state.dispatched) == COUNT + 1);
    assert(atomic_load(&state.seen) == COUNT);
    assert(atomic_load(&state.seen_data_slots) == ((UINT64_C(1) << COUNT) - 1));
    assert(atomic_load(&state.errors) == 0);
    assert(ores_native_actor_scheduler_accepted(scheduler) == COUNT + 1);
    assert(ores_native_actor_scheduler_completed(scheduler) == COUNT + 1);
    assert(ores_native_actor_scheduler_control_completed(scheduler) == 1);
    assert(ores_native_actor_scheduler_rejected(scheduler) == 2);
    assert(ores_native_actor_scheduler_poll_done(
        scheduler, refs[COUNT - 1]));
    assert(ores_native_actor_scheduler_wait_done(
        scheduler, refs[COUNT - 1]));
    for (size_t i = 0; i < COUNT - 1; ++i) {
        assert(ores_native_actor_scheduler_stop(scheduler, refs[i]));
        assert(ores_native_actor_scheduler_poll_done(scheduler, refs[i]));
    }
    ores_native_actor_scheduler_retire(scheduler);
    assert(ores_native_actor_scheduler_try_destroy(scheduler));
}

int main(void) {
    State state = {0};
    state.next = (atomic_int*)calloc(ACTORS, sizeof(atomic_int));
    state.active = (atomic_int*)calloc(ACTORS, sizeof(atomic_int));
    assert(state.next && state.active);
    for (size_t i = 0; i < ACTORS; i++) {
        atomic_init(&state.next[i], 0);
        atomic_init(&state.active[i], 0);
    }
    OresNativeActorScheduler* s = ores_native_actor_scheduler_open_control(
        ACTORS, ROUNDS + 1, 8, data, control, &state);
    assert(s);
    OresNativeActorRef* refs = (OresNativeActorRef*)calloc(ACTORS, sizeof(*refs));
    assert(refs);
    for (size_t a = 0; a < ACTORS; a++) {
        refs[a] = ores_native_actor_scheduler_spawn(s, a);
        assert(refs[a].scheduler_domain != 0);
    }
    for (uint64_t round = 0; round < ROUNDS; round++)
        for (size_t a = 0; a < ACTORS; a++)
            assert(ores_native_actor_scheduler_send(s, refs[a], round));
    assert(ores_native_actor_scheduler_publish_control(
        s, refs[ACTORS - 1], UINT64_C(0xdecafbad)));
    assert(ores_native_actor_scheduler_drain(s));
    assert(ores_native_actor_scheduler_accepted(s) == ACTORS * ROUNDS + 1);
    assert(ores_native_actor_scheduler_completed(s) == ACTORS * ROUNDS + 1);
    assert(ores_native_actor_scheduler_control_completed(s) == 1);
    assert(atomic_load(&state.total) == ACTORS * ROUNDS);
    assert(atomic_load(&state.controls) == 1);
    assert(atomic_load(&state.errors) == 0);
    for (size_t a = 0; a < ACTORS; a++) {
        assert(atomic_load(&state.next[a]) == ROUNDS);
        assert(ores_native_actor_scheduler_stop(s, refs[a]));
        assert(ores_native_actor_scheduler_poll_done(s, refs[a]));
    }
    ores_native_actor_scheduler_retire(s);
    assert(ores_native_actor_scheduler_try_destroy(s));
    free(refs);
    free(state.next);
    free(state.active);

    /* Last bit of the last summary group; most actor lanes remain unspawned.
     * Verify no false hit, missing wakeup or out-of-bounds summary read. */
    State sparse = {0};
    sparse.next = (atomic_int*)calloc(65536, sizeof(atomic_int));
    sparse.active = (atomic_int*)calloc(65536, sizeof(atomic_int));
    assert(sparse.next && sparse.active);
    for (size_t a = 0; a < 65536; ++a) {
        atomic_init(&sparse.next[a], 0);
        atomic_init(&sparse.active[a], 0);
    }
    s = ores_native_actor_scheduler_open(65536, 1, 2, data, &sparse);
    assert(s);
    OresNativeActorRef last = ores_native_actor_scheduler_spawn(s, 65535);
    assert(last.scheduler_domain);
    assert(ores_native_actor_scheduler_send(s, last, 0));
    assert(ores_native_actor_scheduler_drain(s));
    assert(atomic_load(&sparse.total) == 1);
    assert(atomic_load(&sparse.errors) == 0);
    assert(ores_native_actor_scheduler_stop(s, last));
    ores_native_actor_scheduler_retire(s);
    assert(ores_native_actor_scheduler_try_destroy(s));
    free(sparse.next);
    free(sparse.active);
    assert(!ores_native_actor_scheduler_open(65537, 1, 1, data, NULL));
    run_priority_boundary_regression();
    puts("NATIVE_INDEXED_READY_PASS actors=4096 turns=16384 sparse_high_slot=65535 control_precedence=pass");
    return 0;
}
