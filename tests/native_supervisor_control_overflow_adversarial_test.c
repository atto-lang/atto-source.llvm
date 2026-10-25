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
 * This is a characterization of the CURRENT one-slot CONTROL admission API,
 * not a claim of lossless supervisor failure delivery. When A fills CONTROL,
 * B returns 0 and an external trusted owner must retain and retry B. The
 * companion P0 issue requires integrating a durable per-child failure lane
 * before the end-to-end supervision contract can be considered proven.
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
    (void)opaque; (void)actor; (void)payload;
    abort(); /* typed failures, never a user CONTROL envelope */
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
    require(ores_native_actor_scheduler_publish_failure(s.scheduler, s.parent, a));
    /* A valid, distinct child failure is NOT retained by this scheduler. */
    require(!ores_native_actor_scheduler_publish_failure(s.scheduler, s.parent, b));
    require(s.count == 1);
    atomic_store(&s.unblock_first, 1);
    require(ores_native_actor_scheduler_drain(s.scheduler));

    /* CONTROL gets the next parent turn; USER remains FIFO and is not lost. */
    require(s.count == 4);
    require(s.trace[0] == 11 && s.trace[1] == 101
            && s.trace[2] == 12 && s.trace[3] == 13);
    require(ores_native_actor_scheduler_control_completed(s.scheduler) == 1);
    require(atomic_load(&s.peak_parent_active) == 1);

    /* External caller-held retry. Real supervision MUST make this durable. */
    require(ores_native_actor_scheduler_publish_failure(s.scheduler, s.parent, b));
    require(ores_native_actor_scheduler_drain(s.scheduler));
    require(s.count == 5 && s.trace[4] == 102);
    require(ores_native_actor_scheduler_control_completed(s.scheduler) == 2);

    require(ores_native_actor_scheduler_stop(s.scheduler, s.parent));
    require(!ores_native_actor_scheduler_publish_failure(s.scheduler, s.parent, a));
    require(ores_native_actor_scheduler_stop(s.scheduler, s.child_a));
    require(ores_native_actor_scheduler_stop(s.scheduler, s.child_b));
    ores_native_actor_scheduler_retire(s.scheduler);
    require(ores_native_actor_scheduler_try_destroy(s.scheduler));
    puts("NATIVE_SUPERVISOR_CONTROL_OVERFLOW_CHARACTERIZED: retry_required=1");
    return 0;
}
