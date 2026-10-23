#define _POSIX_C_SOURCE 200809L
#include "oreslang/runtime/NativeActorScheduler.h"

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

/*
 * Adversarial, real NativeActorScheduler dispatch test.
 *
 * This is a real scheduler regression for pre-bound distinct child receipts:
 * failure A and failure B must both survive USER saturation, and the parent
 * must dispatch them serially ahead of previously queued USER traffic.
 * This is not source-level supervisor/guest/ABI parity proof.
 */
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

static void require(int condition) {
    if (!condition) abort();
}

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
    require(actor == 0 && payload == 77);
    start_parent_turn(s);
    require(s->count < 8);
    s->trace[s->count++] = 77;
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

    require(ores_native_actor_scheduler_send(s.scheduler, s.parent, 11));
    await_flag(&s.first_started);
    require(ores_native_actor_scheduler_send(s.scheduler, s.parent, 12));
    require(ores_native_actor_scheduler_send(s.scheduler, s.parent, 13));
    require(ores_native_actor_scheduler_try_send(s.scheduler, s.parent, 14)
            == ORES_NATIVE_SEND_MAILBOX_FULL);

    const OresNativeActorTerminalFailure a =
        {ORES_NATIVE_CONTROL_PANIC, s.child_a, 99, 101};
    const OresNativeActorTerminalFailure b =
        {ORES_NATIVE_CONTROL_PANIC, s.child_b, 99, 102};
    require(ores_native_actor_scheduler_publish_control(s.scheduler, s.parent, 77));
    require(ores_native_actor_scheduler_publish_failure(s.scheduler, s.parent, a));
    require(ores_native_actor_scheduler_publish_failure(s.scheduler, s.parent, b));
    /* Duplicate publication is rejected without overwriting a receipt. */
    require(!ores_native_actor_scheduler_publish_failure(s.scheduler, s.parent, a));
    require(s.count == 1);
    atomic_store(&s.unblock_first, 1);
    require(ores_native_actor_scheduler_drain(s.scheduler));

    /* Ordinary CONTROL precedes both distinct typed failures, then FIFO USER. */
    require(s.count == 6);
    require(s.trace[0] == 11 && s.trace[1] == 77
            && s.trace[2] == 101 && s.trace[3] == 102
            && s.trace[4] == 12 && s.trace[5] == 13);
    require(ores_native_actor_scheduler_control_completed(s.scheduler) == 3);
    require(atomic_load(&s.peak_parent_active) == 1);
    require(!ores_native_actor_scheduler_publish_failure(s.scheduler, s.parent, b));

    require(ores_native_actor_scheduler_stop(s.scheduler, s.parent));
    require(!ores_native_actor_scheduler_publish_failure(s.scheduler, s.parent, a));
    require(ores_native_actor_scheduler_stop(s.scheduler, s.child_a));
    require(ores_native_actor_scheduler_stop(s.scheduler, s.child_b));
    ores_native_actor_scheduler_retire(s.scheduler);
    require(ores_native_actor_scheduler_try_destroy(s.scheduler));
    puts("NATIVE_SUPERVISOR_CONTROL_OVERFLOW_PASS: distinct=2 retained=2");
    return 0;
}
