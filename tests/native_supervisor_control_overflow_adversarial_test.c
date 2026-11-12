#define _POSIX_C_SOURCE 200809L
#include "oreslang/runtime/NativeActorScheduler.h"

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

/* Native real-scheduler, no-manual-retry per-child CONTROL receipt proof. */
typedef struct {
    OresNativeActorScheduler* scheduler;
    OresNativeActorRef parent;
    OresNativeActorRef child_a;
    OresNativeActorRef child_b;
    atomic_int first_started;
    atomic_int unblock_first;
    atomic_int parent_active;
    atomic_int peak_parent_active;
    int trace[8];
    int count;
} State;

static void require_at(int condition, int line) {
    if (!condition) {
        fprintf(stderr, "NATIVE_SUPERVISOR_ASSERTION_FAILED line=%d\\n", line);
        fflush(stderr);
        abort();
    }
}
#define require(condition) require_at(!!(condition), __LINE__)

static void tick(void) {
    struct timespec delay = {0, 1000000};
    nanosleep(&delay, NULL);
}

static void await_flag(atomic_int* flag) {
    for (int i = 0; i < 5000; ++i) {
        if (atomic_load(flag)) return;
        tick();
    }
    abort();
}

static void start_parent_turn(State* s) {
    require(ores_native_actor_scheduler_is_current_actor(s->scheduler, s->parent));
    int active = atomic_fetch_add(&s->parent_active, 1) + 1;
    int previous = atomic_load(&s->peak_parent_active);
    while (active > previous &&
           !atomic_compare_exchange_weak(&s->peak_parent_active, &previous, active)) { }
    require(active == 1);
}

static void end_parent_turn(State* s) {
    require(atomic_fetch_sub(&s->parent_active, 1) == 1);
}

static void on_data(void* opaque, size_t actor, uint64_t payload) {
    State* s = (State*)opaque;
    require(actor == 0);
    start_parent_turn(s);
    if (payload == 11) {
        require(s->count == 0);
        s->trace[s->count++] = 11;
        atomic_store(&s->first_started, 1);
        while (!atomic_load(&s->unblock_first)) tick();
    } else {
        require(payload == 12 || payload == 13);
        require(s->count < 8);
        s->trace[s->count++] = (int)payload;
    }
    end_parent_turn(s);
}

static void on_control(void* opaque, size_t actor, uint64_t payload) {
    State* s = (State*)opaque;
    require(actor == 0 && payload == 777);
    start_parent_turn(s);
    require(s->count < 8);
    s->trace[s->count++] = (int)payload;
    end_parent_turn(s);
}

static int ref_matches(OresNativeActorRef left, OresNativeActorRef right) {
    return left.scheduler_domain == right.scheduler_domain
        && left.actor_slot == right.actor_slot
        && left.generation == right.generation;
}

static void on_panic(void* opaque, size_t actor, OresNativeActorTerminalFailure failure) {
    State* s = (State*)opaque;
    require(actor == 0);
    require(failure.kind == ORES_NATIVE_CONTROL_PANIC);
    require(failure.failure_code == 99);
    start_parent_turn(s);
    require(s->count < 8);
    if (ref_matches(failure.child, s->child_a)) {
        require(failure.correlation == 101);
        s->trace[s->count++] = 101;
    } else {
        require(ref_matches(failure.child, s->child_b));
        require(failure.correlation == 102);
        s->trace[s->count++] = 102;
    }
    end_parent_turn(s);
}


/* A single parent USER turn stays active while every child takes its own
 * bounded, preallocated failure receipt. No per-failure allocation or retry.
 * The check explicitly pins physical DONE until all receipts are ACKed. */
enum { STRESS_CHILDREN = 1000 };
typedef struct {
    OresNativeActorScheduler* scheduler;
    OresNativeActorRef parent;
    OresNativeActorRef children[STRESS_CHILDREN];
    unsigned char seen[STRESS_CHILDREN];
    size_t notified;
    atomic_int parent_entered;
    atomic_int release_parent;
} StressState;

static void stress_on_data(void* opaque, size_t actor, uint64_t payload) {
    StressState* state = (StressState*)opaque;
    require(actor == 0 && (payload == 7 || payload == 8));
    require(ores_native_actor_scheduler_is_current_actor(
            state->scheduler, state->parent));
    if (payload == 7) {
        atomic_store(&state->parent_entered, 1);
        while (!atomic_load(&state->release_parent)) tick();
    }
}

static void stress_on_panic(
        void* opaque, size_t actor, OresNativeActorTerminalFailure failure) {
    StressState* state = (StressState*)opaque;
    require(actor == 0);
    require(ores_native_actor_scheduler_is_current_actor(
            state->scheduler, state->parent));
    require(failure.kind == ORES_NATIVE_CONTROL_PANIC
            && failure.failure_code == 99);
    require(failure.child.actor_slot >= 1
            && failure.child.actor_slot <= STRESS_CHILDREN);
    const size_t index = (size_t)failure.child.actor_slot - 1;
    require(ref_matches(failure.child, state->children[index]));
    require(failure.correlation == (uint64_t)index + 123);
    require(!state->seen[index]);
    state->seen[index] = 1;
    ++state->notified;
}

static void stress_thousand_failures_without_retries(void) {
    StressState* state = (StressState*)calloc(1, sizeof(StressState));
    require(state != NULL);
    state->scheduler = ores_native_actor_scheduler_open_typed_control(
            STRESS_CHILDREN + 1, 2, 2,
            stress_on_data, NULL, stress_on_panic, state);
    require(state->scheduler != NULL);
    state->parent = ores_native_actor_scheduler_spawn(state->scheduler, 0);
    require(state->parent.scheduler_domain != 0);
    for (size_t i = 0; i < STRESS_CHILDREN; ++i) {
        state->children[i] = ores_native_actor_scheduler_spawn(
                state->scheduler, i + 1);
        require(state->children[i].scheduler_domain != 0);
        require(ores_native_actor_scheduler_bind_supervisor(
                state->scheduler, state->parent, state->children[i]));
    }
    require(ores_native_actor_scheduler_send(
            state->scheduler, state->parent, 7));
    await_flag(&state->parent_entered);
    require(ores_native_actor_scheduler_send(
            state->scheduler, state->parent, 8));
    require(ores_native_actor_scheduler_try_send(
            state->scheduler, state->parent, 9)
            == ORES_NATIVE_SEND_MAILBOX_FULL);
    for (size_t i = 0; i < STRESS_CHILDREN; ++i) {
        OresNativeActorTerminalFailure failure = {
            ORES_NATIVE_CONTROL_PANIC, state->children[i], 99, (uint64_t)i + 123
        };
        require(ores_native_actor_scheduler_publish_failure(
                state->scheduler, state->parent, failure));
        require(!ores_native_actor_scheduler_publish_failure(
                state->scheduler, state->parent, failure));
        require(ores_native_actor_scheduler_stop(
                state->scheduler, state->children[i]));
        require(!ores_native_actor_scheduler_poll_done(
                state->scheduler, state->children[i]));
    }
    atomic_store(&state->release_parent, 1);
    require(ores_native_actor_scheduler_drain(state->scheduler));
    require(state->notified == STRESS_CHILDREN);
    require(ores_native_actor_scheduler_control_completed(state->scheduler)
            == STRESS_CHILDREN);
    for (size_t i = 0; i < STRESS_CHILDREN; ++i) {
        require(state->seen[i]);
        require(ores_native_actor_scheduler_poll_done(
                state->scheduler, state->children[i]));
    }
    require(ores_native_actor_scheduler_stop(state->scheduler, state->parent));
    ores_native_actor_scheduler_retire(state->scheduler);
    require(ores_native_actor_scheduler_try_destroy(state->scheduler));
    free(state);
}

int main(void) {
    State s = {0};
    s.scheduler = ores_native_actor_scheduler_open_typed_control(
        3, 3, 2, on_data, on_control, on_panic, &s);
    require(s.scheduler != NULL);
    s.parent = ores_native_actor_scheduler_spawn(s.scheduler, 0);
    s.child_a = ores_native_actor_scheduler_spawn(s.scheduler, 1);
    s.child_b = ores_native_actor_scheduler_spawn(s.scheduler, 2);
    require(s.parent.scheduler_domain && s.child_a.scheduler_domain
            && s.child_b.scheduler_domain);
    require(ores_native_actor_scheduler_bind_supervisor(
            s.scheduler, s.parent, s.child_a));
    require(ores_native_actor_scheduler_bind_supervisor(
            s.scheduler, s.parent, s.child_b));
    require(!ores_native_actor_scheduler_bind_supervisor(
            s.scheduler, s.parent, s.child_a));
    require(!ores_native_actor_scheduler_bind_supervisor(
            s.scheduler, s.child_a, s.parent));

    require(ores_native_actor_scheduler_send(s.scheduler, s.parent, 11));
    await_flag(&s.first_started);
    require(ores_native_actor_scheduler_send(s.scheduler, s.parent, 12));
    require(ores_native_actor_scheduler_send(s.scheduler, s.parent, 13));
    require(ores_native_actor_scheduler_try_send(s.scheduler, s.parent, 14)
            == ORES_NATIVE_SEND_MAILBOX_FULL);

    /* Generic CONTROL must be independently reservable even when USER
     * is saturated; child failure receipts may coexist with its slot. */
    require(ores_native_actor_scheduler_publish_control(s.scheduler, s.parent, 777));
    require(!ores_native_actor_scheduler_publish_control(s.scheduler, s.parent, 778));

    const OresNativeActorTerminalFailure a =
        {ORES_NATIVE_CONTROL_PANIC, s.child_a, 99, 101};
    const OresNativeActorTerminalFailure b =
        {ORES_NATIVE_CONTROL_PANIC, s.child_b, 99, 102};
    require(ores_native_actor_scheduler_publish_failure(s.scheduler, s.parent, a));
    /* Separate children each own a terminal receipt, independent of DATA. */
    require(ores_native_actor_scheduler_publish_failure(s.scheduler, s.parent, b));
    require(!ores_native_actor_scheduler_publish_failure(s.scheduler, s.parent, a));
    require(!ores_native_actor_scheduler_publish_failure(s.scheduler, s.child_a, b));
    /* Child physical DONE is not observable while its terminal receipt
     * has not been acknowledged by the owning supervisor. */
    require(ores_native_actor_scheduler_stop(s.scheduler, s.child_a));
    require(!ores_native_actor_scheduler_poll_done(s.scheduler, s.child_a));
    require(s.count == 1);
    atomic_store(&s.unblock_first, 1);
    require(ores_native_actor_scheduler_drain(s.scheduler));

    /* CONTROL gets the next parent turn; USER remains FIFO and is not lost. */
    require(s.count == 6);
    require(s.trace[0] == 11 && s.trace[1] == 101
            && s.trace[2] == 102 && s.trace[3] == 777
            && s.trace[4] == 12 && s.trace[5] == 13);
    require(ores_native_actor_scheduler_control_completed(s.scheduler) == 3);
    require(atomic_load(&s.peak_parent_active) == 1);
    require(ores_native_actor_scheduler_poll_done(s.scheduler, s.child_a));
    require(!ores_native_actor_scheduler_publish_failure(s.scheduler, s.parent, b));

    require(ores_native_actor_scheduler_stop(s.scheduler, s.parent));
    require(!ores_native_actor_scheduler_publish_failure(s.scheduler, s.parent, a));
    require(ores_native_actor_scheduler_stop(s.scheduler, s.child_b));
    ores_native_actor_scheduler_retire(s.scheduler);
    require(ores_native_actor_scheduler_try_destroy(s.scheduler));
    stress_thousand_failures_without_retries();
    puts("NATIVE_SUPERVISOR_CONTROL_LOSSLESS: distinct_children=1000 manual_retry=0");
    return 0;
}
