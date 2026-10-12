#define _POSIX_C_SOURCE 200809L
#include "oreslang/runtime/NativeActorScheduler.h"
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <time.h>

typedef struct {
    OresNativeActorScheduler* scheduler;
    OresNativeActorRef parent, child;
    atomic_int data_active, parent_peak, failures;
    atomic_int parent_started, release_parent, panic_count, child_count;
    atomic_int child_during_parent, panic_during_parent;
} State;
static void require(int cond) { if (!cond) abort(); }
static void tick(void) {
    struct timespec pause = {0, 1000000};
    nanosleep(&pause, NULL);
}
static void until(atomic_int* flag) {
    for (int n = 0; n < 5000; ++n) {
        if (atomic_load(flag)) return;
        tick();
    }
    abort();
}
static void check_actor(State* s, OresNativeActorRef actor, OresNativeActorRef other) {
    if (!ores_native_actor_scheduler_is_current_actor(s->scheduler, actor)
        || ores_native_actor_scheduler_is_current_actor(s->scheduler, other))
        atomic_fetch_add(&s->failures, 1);
}
static void on_data(void* opaque, size_t actor, uint64_t payload) {
    State* s = (State*)opaque;
    if (actor == 0) {
        require(payload == 13);
        check_actor(s, s->parent, s->child);
        int active = atomic_fetch_add(&s->data_active, 1) + 1;
        int peak = atomic_load(&s->parent_peak);
        while (active > peak
               && !atomic_compare_exchange_weak(&s->parent_peak, &peak, active)) { }
        if (active > 1) atomic_fetch_add(&s->failures, 1);
        atomic_store(&s->parent_started, 1);
        while (!atomic_load(&s->release_parent)) tick();
        require(atomic_fetch_sub(&s->data_active, 1) == 1);
    } else {
        require(actor == 1 && payload == 21);
        check_actor(s, s->child, s->parent);
        if (atomic_load(&s->data_active) == 1)
            atomic_store(&s->child_during_parent, 1);
        atomic_fetch_add(&s->child_count, 1);
    }
}
static void on_future(void* opaque, size_t actor, uint64_t payload) {
    (void)opaque; (void)actor; (void)payload;
    abort(); /* test only publishes typed panic, never ordinary CONTROL */
}
static void on_panic(void* opaque, size_t actor, OresNativeActorTerminalFailure event) {
    State* s = (State*)opaque;
    require(actor == 0);
    require(event.kind == ORES_NATIVE_CONTROL_PANIC);
    require(event.child.scheduler_domain == s->child.scheduler_domain);
    require(event.child.actor_slot == s->child.actor_slot);
    require(event.child.generation == s->child.generation);
    require(event.failure_code == 92 && event.correlation == 104);
    check_actor(s, s->parent, s->child);
    if (atomic_load(&s->data_active) != 0)
        atomic_store(&s->panic_during_parent, 1);
    atomic_fetch_add(&s->panic_count, 1);
}
int main(void) {
    State s = {0};
    s.scheduler = ores_native_actor_scheduler_open_typed_control(
        2, 4, 2, on_data, on_future, on_panic, &s);
    require(s.scheduler != NULL);
    s.parent = ores_native_actor_scheduler_spawn(s.scheduler, 0);
    s.child = ores_native_actor_scheduler_spawn(s.scheduler, 1);
    require(s.parent.scheduler_domain != 0 && s.child.scheduler_domain != 0);
    require(!ores_native_actor_scheduler_is_current_actor(s.scheduler, s.parent));

    require(ores_native_actor_scheduler_send(s.scheduler, s.parent, 13));
    until(&s.parent_started);

    OresNativeActorTerminalFailure event = {
        ORES_NATIVE_CONTROL_PANIC, s.child, 92, 104
    };
    require(ores_native_actor_scheduler_publish_failure(s.scheduler, s.parent, event));
    require(ores_native_actor_scheduler_send(s.scheduler, s.child, 21));
    until(&s.child_count);
    require(atomic_load(&s.child_during_parent) == 1);
    require(atomic_load(&s.panic_count) == 0);
    require(!ores_native_actor_scheduler_is_current_actor(s.scheduler, s.parent));

    atomic_store(&s.release_parent, 1);
    require(ores_native_actor_scheduler_drain(s.scheduler));
    require(atomic_load(&s.panic_count) == 1);
    require(atomic_load(&s.panic_during_parent) == 0);
    require(atomic_load(&s.parent_peak) == 1);
    require(atomic_load(&s.failures) == 0);
    require(ores_native_actor_scheduler_control_completed(s.scheduler) == 1);
    require(ores_native_actor_scheduler_stop(s.scheduler, s.parent));
    require(ores_native_actor_scheduler_stop(s.scheduler, s.child));
    ores_native_actor_scheduler_retire(s.scheduler);
    require(ores_native_actor_scheduler_try_destroy(s.scheduler));
    return 0;
}
