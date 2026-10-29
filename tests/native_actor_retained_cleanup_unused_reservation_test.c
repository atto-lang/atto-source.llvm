#define _POSIX_C_SOURCE 200809L
#include "oreslang/runtime/NativeActorScheduler.h"
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, \
    "CLEANUP_RESERVATION_RELEASE_FAIL %s:%d %s\n",__FILE__,__LINE__,#x); abort(); } } while (0)

typedef struct {
    OresNativeActorScheduler* scheduler;
    OresNativeActorRef actor;
    OresNativeRetainedDataTicket retained;
    OresNativeWakeTicket wake;
    atomic_int suspended, completed, cleanup_calls;
} Fixture;
static void wait_until(atomic_int* ready) {
    for (int i=0;i<8000;++i) {
        if (atomic_load_explicit(ready,memory_order_acquire)) return;
        const struct timespec pause={0,1000000};
        nanosleep(&pause,NULL);
    }
    fprintf(stderr,"cleanup reservation release timeout\n"); abort();
}
static int cleanup(void* raw,size_t actor,OresNativeRetainedDataTicket ticket) {
    Fixture* f=(Fixture*)raw;
    (void)ticket;
    CHECK(actor==0);
    atomic_fetch_add_explicit(&f->cleanup_calls,1,memory_order_relaxed);
    /* Normal continuation completion must cancel this reservation first;
     * a cleanup callback here is an invalid lifecycle transition. */
    return 0;
}
static void data(void* raw,size_t actor,uint64_t payload) {
    Fixture* f=(Fixture*)raw;
    CHECK(actor==0 && payload==100);
    CHECK(ores_native_actor_scheduler_is_current_actor(f->scheduler,f->actor));
    CHECK(ores_native_actor_scheduler_retain_current_data(
        f->scheduler,&f->retained));
    CHECK(ores_native_actor_scheduler_reserve_wake(
        f->scheduler,f->actor,f->actor,1,1,1,1,&f->wake));
    atomic_store_explicit(&f->suspended,1,memory_order_release);
}
static void wake_callback(void* raw,size_t actor,
        OresNativeWakeTicket ticket,OresNativeWakeOutcome outcome) {
    Fixture* f=(Fixture*)raw;
    CHECK(actor==0 && outcome==ORES_NATIVE_WAKE_READY);
    CHECK(ores_native_actor_scheduler_is_current_wake(f->scheduler,ticket));
    CHECK(ores_native_actor_scheduler_is_current_actor(
        f->scheduler,f->actor));
    CHECK(ticket.slot_generation==f->wake.slot_generation);
    CHECK(ores_native_actor_scheduler_complete_retained_data(
        f->scheduler,f->retained));
    CHECK(!ores_native_actor_scheduler_complete_retained_data(
        f->scheduler,f->retained));
    atomic_store_explicit(&f->completed,1,memory_order_release);
}
int main(void) {
    Fixture f={0};
    f.scheduler=ores_native_actor_scheduler_open_with_wakes(
        1,1,1,data,NULL,NULL,wake_callback,&f);
    CHECK(f.scheduler);
    CHECK(ores_native_actor_scheduler_configure_retained_cleanup(
        f.scheduler,cleanup));
    f.actor=ores_native_actor_scheduler_spawn(f.scheduler,0);
    CHECK(f.actor.scheduler_domain);
    /* This scheduler has no authenticated cross-scheduler bridge: foreign
     * domains must fail at RESERVE, before minting any escrow credit. */
    OresNativeActorRef foreign=f.actor;
    foreign.scheduler_domain=(f.actor.scheduler_domain==1) ? 2 : 1;
    OresNativeWakeTicket invalid={0};
    invalid.slot_generation=UINT64_C(123);
    CHECK(!ores_native_actor_scheduler_reserve_wake(
        f.scheduler,f.actor,foreign,1,1,1,1,&invalid));
    CHECK(invalid.slot_generation==0 && invalid.producer.scheduler_domain==0);
    CHECK(ores_native_actor_scheduler_outstanding_wake_loans(
        f.scheduler,f.actor)==0);
    CHECK(ores_native_actor_scheduler_accepted(f.scheduler)==0);
    /* Also reject an otherwise well-formed local target with a forged
     * generation, without consuming a reservation or starting a turn. */
    OresNativeActorRef stale=f.actor;
    ++stale.generation;
    CHECK(stale.generation!=0);
    CHECK(!ores_native_actor_scheduler_reserve_wake(
        f.scheduler,f.actor,stale,1,1,1,1,&invalid));
    CHECK(ores_native_actor_scheduler_outstanding_wake_loans(
        f.scheduler,f.actor)==0);
    CHECK(ores_native_actor_scheduler_send(f.scheduler,f.actor,100));
    wait_until(&f.suspended);
    CHECK(ores_native_actor_scheduler_retained_data_count(
        f.scheduler,f.actor)==1);
    CHECK(ores_native_actor_scheduler_transfer_wake(f.scheduler,f.wake));
    CHECK(ores_native_actor_scheduler_publish_wake(
        f.scheduler,f.wake,ORES_NATIVE_WAKE_READY)==ORES_NATIVE_WAKE_PUBLISHED);
    wait_until(&f.completed);
    CHECK(ores_native_actor_scheduler_drain(f.scheduler));
    /* A completed wake ticket is not reusable as a new transfer, publish,
     * cancel or escrow release of any later-generation slot. */
    CHECK(!ores_native_actor_scheduler_transfer_wake(f.scheduler,f.wake));
    CHECK(!ores_native_actor_scheduler_cancel_wake(f.scheduler,f.wake));
    CHECK(ores_native_actor_scheduler_publish_wake(
        f.scheduler,f.wake,ORES_NATIVE_WAKE_READY)==ORES_NATIVE_WAKE_INVALID);
    CHECK(!ores_native_actor_scheduler_retained_data_count(
        f.scheduler,f.actor));
    CHECK(atomic_load(&f.cleanup_calls)==0);
    CHECK(ores_native_actor_scheduler_accepted(f.scheduler)==2);
    CHECK(ores_native_actor_scheduler_completed(f.scheduler)==2);
    CHECK(ores_native_actor_scheduler_stop_sealed(
        f.scheduler,f.actor));
    CHECK(ores_native_actor_scheduler_wait_done(f.scheduler,f.actor));
    OresNativeActorTerminalState state={0};
    CHECK(ores_native_actor_scheduler_terminal_state(
        f.scheduler,f.actor,&state));
    CHECK(state.accepted_user_messages==1);
    CHECK(state.processed_user_messages==1);
    CHECK(state.abandoned_user_messages==0);
    CHECK(atomic_load(&f.cleanup_calls)==0);
    ores_native_actor_scheduler_retire(f.scheduler);
    CHECK(ores_native_actor_scheduler_try_destroy(f.scheduler));
    puts("NATIVE_CLEANUP_RESERVATION_RELEASE_PASS");
    return 0;
}
