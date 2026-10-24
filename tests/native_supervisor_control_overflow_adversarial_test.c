#define _POSIX_C_SOURCE 200809L
#include "oreslang/runtime/NativeActorScheduler.h"

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

/*
 * Real NativeActorScheduler lossless terminal-failure regression.
 *
 * Typed child failures own one bounded receipt per child slot and may queue
 * while the parent is executing DATA, while its ordinary CONTROL slot is busy,
 * or while other sibling failures are pending. Delivery remains serialized on
 * the parent's actor lane. Exact retries are idempotent; conflicting rewrites
 * of an already-owned child receipt reject.
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

typedef struct {
    OresNativeActorScheduler* scheduler;
    OresNativeActorRef parent;
    OresNativeActorRef children[1000];
    atomic_int parent_started;
    atomic_int unblock_parent;
    unsigned char seen[1001];
    size_t delivered;
} StressState;

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

static int ref_matches(OresNativeActorRef left, OresNativeActorRef right) {
    return left.scheduler_domain == right.scheduler_domain
        && left.actor_slot == right.actor_slot
        && left.generation == right.generation;
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

static void two_child_case(void) {
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
    /* Occupy the independent ordinary CONTROL slot before terminal failures. */
    require(ores_native_actor_scheduler_publish_control(s.scheduler, s.parent, 77));
    require(!ores_native_actor_scheduler_publish_control(s.scheduler, s.parent, 78));

    const OresNativeActorTerminalFailure a =
        {ORES_NATIVE_CONTROL_PANIC, s.child_a, 99, 101};
    const OresNativeActorTerminalFailure b =
        {ORES_NATIVE_CONTROL_PANIC, s.child_b, 99, 102};
    require(ores_native_actor_scheduler_publish_failure(s.scheduler, s.parent, a));
    require(ores_native_actor_scheduler_publish_failure(s.scheduler, s.parent, b));

    /* Exact retry owns no second execution slot and cannot double-deliver. */
    require(ores_native_actor_scheduler_publish_failure(s.scheduler, s.parent, b));
    const OresNativeActorTerminalFailure conflicting =
        {ORES_NATIVE_CONTROL_PANIC, s.child_b, 99, 999};
    require(!ores_native_actor_scheduler_publish_failure(
        s.scheduler, s.parent, conflicting));

    require(s.count == 1);
    atomic_store(&s.unblock_first, 1);
    require(ores_native_actor_scheduler_drain(s.scheduler));

    /* Both terminal failures outrank already-admitted USER work, stay FIFO,
     * and never overlap the parent's active DATA turn. */
    require(s.count == 6);
    require(s.trace[0] == 11 && s.trace[1] == 101 && s.trace[2] == 102
            && s.trace[3] == 77 && s.trace[4] == 12 && s.trace[5] == 13);
    require(ores_native_actor_scheduler_control_completed(s.scheduler) == 3);
    require(atomic_load(&s.peak_parent_active) == 1);

    /* Delivered exact retry remains idempotent and creates no new callback. */
    require(ores_native_actor_scheduler_publish_failure(s.scheduler, s.parent, a));
    require(ores_native_actor_scheduler_drain(s.scheduler));
    require(s.count == 6);
    require(ores_native_actor_scheduler_control_completed(s.scheduler) == 3);

    require(ores_native_actor_scheduler_stop(s.scheduler, s.parent));
    require(!ores_native_actor_scheduler_publish_failure(s.scheduler, s.parent, a));
    require(ores_native_actor_scheduler_stop(s.scheduler, s.child_a));
    require(ores_native_actor_scheduler_stop(s.scheduler, s.child_b));
    ores_native_actor_scheduler_retire(s.scheduler);
    require(ores_native_actor_scheduler_try_destroy(s.scheduler));
}

static void stress_data(void* opaque, size_t actor, uint64_t payload) {
    StressState* s = (StressState*)opaque;
    require(actor == 0 && payload == 777);
    require(ores_native_actor_scheduler_is_current_actor(s->scheduler, s->parent));
    atomic_store(&s->parent_started, 1);
    while (!atomic_load(&s->unblock_parent)) tick();
}

static void stress_control(void* opaque, size_t actor, uint64_t payload) {
    (void)opaque; (void)actor; (void)payload;
    abort();
}

static void stress_panic(
        void* opaque, size_t actor, OresNativeActorTerminalFailure failure) {
    StressState* s = (StressState*)opaque;
    require(actor == 0);
    require(ores_native_actor_scheduler_is_current_actor(s->scheduler, s->parent));
    require(failure.kind == ORES_NATIVE_CONTROL_PANIC);
    require(failure.failure_code == 77);
    require(failure.correlation >= 1 && failure.correlation <= 1000);
    const size_t index = (size_t)failure.correlation;
    require(ref_matches(failure.child, s->children[index - 1]));
    require(failure.child.actor_slot == failure.correlation);
    require(s->seen[index] == 0);
    s->seen[index] = 1;
    ++s->delivered;
}

static void thousand_sibling_case(void) {
    StressState s = {0};
    s.scheduler = ores_native_actor_scheduler_open_typed_control(
        1001, 1, 1, stress_data, stress_control, stress_panic, &s);
    require(s.scheduler != NULL);
    s.parent = ores_native_actor_scheduler_spawn(s.scheduler, 0);
    require(s.parent.scheduler_domain != 0);
    for (size_t i = 0; i < 1000; ++i) {
        s.children[i] = ores_native_actor_scheduler_spawn(s.scheduler, i + 1);
        require(s.children[i].scheduler_domain == s.parent.scheduler_domain);
    }

    require(ores_native_actor_scheduler_send(s.scheduler, s.parent, 777));
    await_flag(&s.parent_started);

    for (size_t i = 0; i < 1000; ++i) {
        OresNativeActorTerminalFailure failure = {
            ORES_NATIVE_CONTROL_PANIC, s.children[i], 77, i + 1
        };
        require(ores_native_actor_scheduler_publish_failure(
            s.scheduler, s.parent, failure));
    }

    /* A saturated parent retains all sibling receipts without producer retry. */
    OresNativeActorTerminalFailure duplicate = {
        ORES_NATIVE_CONTROL_PANIC, s.children[499], 77, 500
    };
    require(ores_native_actor_scheduler_publish_failure(
        s.scheduler, s.parent, duplicate));
    duplicate.failure_code = 78;
    require(!ores_native_actor_scheduler_publish_failure(
        s.scheduler, s.parent, duplicate));

    atomic_store(&s.unblock_parent, 1);
    require(ores_native_actor_scheduler_drain(s.scheduler));
    require(s.delivered == 1000);
    require(ores_native_actor_scheduler_control_completed(s.scheduler) == 1000);
    for (size_t i = 1; i <= 1000; ++i) require(s.seen[i] == 1);

    require(ores_native_actor_scheduler_stop(s.scheduler, s.parent));
    for (size_t i = 0; i < 1000; ++i)
        require(ores_native_actor_scheduler_stop(s.scheduler, s.children[i]));
    ores_native_actor_scheduler_retire(s.scheduler);
    require(ores_native_actor_scheduler_try_destroy(s.scheduler));
}

int main(void) {
    two_child_case();
    thousand_sibling_case();
    puts("NATIVE_SUPERVISOR_FAILURE_RECEIPTS_PASS: siblings=1000 retry_required=0");
    return 0;
}
