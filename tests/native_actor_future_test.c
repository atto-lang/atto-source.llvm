#define _POSIX_C_SOURCE 200809L
#include "oreslang/runtime/NativeActorScheduler.h"
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
typedef struct {
    OresNativeActorScheduler* scheduler;
    OresNativeActorRef parent, child;
    atomic_int parent_started, unblock_parent, init_called;
    atomic_int order, bad;
} State;
static void check(int yes) { if (!yes) abort(); }
static void nap(void) {
    struct timespec t = {0,1000000};
    nanosleep(&t, NULL);
}
static void wait_flag(atomic_int* flag) {
    for (int i = 0; i < 4000; ++i) {
        if (atomic_load(flag)) return;
        nap();
    }
    abort();
}
static void regular(void* raw, size_t slot, uint64_t value) {
    State* s = (State*)raw;
    if (slot == 0 && value == 10) {
        if (atomic_fetch_add(&s->order,1) != 0) atomic_fetch_add(&s->bad,1);
        atomic_store(&s->parent_started,1);
        while (!atomic_load(&s->unblock_parent)) nap();
    } else if (slot == 0 && value == 20) {
        if (atomic_fetch_add(&s->order,1) != 3) atomic_fetch_add(&s->bad,1);
    } else if (slot == 1 && value == 1) {
        if (!ores_native_actor_scheduler_mark_ready(s->scheduler,s->child))
            atomic_fetch_add(&s->bad,1);
        atomic_store(&s->init_called,1);
    } else abort();
}
static void control(void* raw,size_t parent,OresNativeActorControlKind kind,
                    OresNativeActorRef child,uint64_t ticket) {
    State* s=(State*)raw;
    if (parent != 0 || child.actor_slot != s->child.actor_slot
        || child.scheduler_domain != s->child.scheduler_domain
        || child.generation != s->child.generation)
        atomic_fetch_add(&s->bad,1);
    if (kind == ORES_ACTOR_CONTROL_READY) {
        if (ticket != 111 || atomic_fetch_add(&s->order,1) != 1)
            atomic_fetch_add(&s->bad,1);
    } else if (kind == ORES_ACTOR_CONTROL_DONE) {
        if (ticket != 222 || atomic_fetch_add(&s->order,1) != 2)
            atomic_fetch_add(&s->bad,1);
    } else atomic_fetch_add(&s->bad,1);
}
int main(void) {
    State s={0};
    s.scheduler=ores_native_actor_scheduler_open(2,2,3,regular,&s);
    check(s.scheduler);
    check(ores_native_actor_scheduler_set_control_handler(s.scheduler,control));
    s.parent=ores_native_actor_scheduler_spawn(s.scheduler,0);
    s.child=ores_native_actor_scheduler_spawn_pending(s.scheduler,1);
    check(s.parent.scheduler_domain && s.child.scheduler_domain);
    const OresNativeActorFuture ready=ores_native_actor_scheduler_get_ready(s.scheduler,s.child);
    const OresNativeActorFuture done=ores_native_actor_scheduler_get_done(s.scheduler,s.child);
    check(ores_native_actor_future_is_complete(s.scheduler,ready)==0);
    check(ores_native_actor_future_is_complete(s.scheduler,done)==0);
    check(!ores_native_actor_scheduler_mark_ready(s.scheduler,s.child)); /* no carrier */
    check(!ores_native_actor_scheduler_mark_finalized(s.scheduler,s.child)); /* too early */
    check(ores_native_actor_future_subscribe(s.scheduler,ready,s.parent,111));
    check(ores_native_actor_future_subscribe(s.scheduler,done,s.parent,222));
    check(!ores_native_actor_future_subscribe(s.scheduler,ready,s.parent,333));
    check(ores_native_actor_scheduler_send(s.scheduler,s.parent,10));
    wait_flag(&s.parent_started);
    check(ores_native_actor_scheduler_send(s.scheduler,s.parent,20));
    check(ores_native_actor_scheduler_send(s.scheduler,s.child,1));
    wait_flag(&s.init_called);
    check(ores_native_actor_future_is_complete(s.scheduler,ready)==1);
    check(ores_native_actor_future_is_complete(s.scheduler,done)==0);
    check(ores_native_actor_scheduler_stop(s.scheduler,s.child));
    for (int k=0; k<4000 && !ores_native_actor_scheduler_poll_done(s.scheduler,s.child); ++k)
        nap();
    check(ores_native_actor_scheduler_poll_done(s.scheduler,s.child));
    /* In a real actor group, this call is made only after actor-owned arenas,
     * native handles and defer/finally have been physically finalized. */
    check(ores_native_actor_scheduler_mark_finalized(s.scheduler,s.child));
    check(ores_native_actor_future_is_complete(s.scheduler,done)==1);
    check(!ores_native_actor_scheduler_mark_finalized(s.scheduler,s.child));
    check(ores_native_actor_scheduler_control_accepted(s.scheduler)==1);
    check(ores_native_actor_scheduler_control_completed(s.scheduler)==0);
    atomic_store(&s.unblock_parent,1);
    check(ores_native_actor_scheduler_drain(s.scheduler));
    check(atomic_load(&s.order)==4 && atomic_load(&s.bad)==0);
    check(ores_native_actor_scheduler_control_accepted(s.scheduler)==2);
    check(ores_native_actor_scheduler_control_completed(s.scheduler)==2);
    check(ores_native_actor_scheduler_stop(s.scheduler,s.parent));
    ores_native_actor_scheduler_retire(s.scheduler);
    check(ores_native_actor_scheduler_try_destroy(s.scheduler));
    puts("NATIVE_ACTOR_FUTURE_PASS ready=1 done=1 deferred_control=1 parent_single_flight=1");
    return 0;
}
