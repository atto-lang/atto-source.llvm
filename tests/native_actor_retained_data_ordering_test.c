#define _POSIX_C_SOURCE 200809L
#include "oreslang/runtime/NativeActorScheduler.h"
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, \
    "RETAINED_ORDER_FAIL %s:%d: %s\n",__FILE__,__LINE__,#x); abort(); } } while (0)

typedef struct {
    OresNativeActorScheduler* scheduler;
    OresNativeActorRef actor[2];
    OresNativeRetainedDataTicket retained;
    OresNativeWakeTicket wake;
    atomic_int first_suspended, later_ran, other_progress, resumed;
} State;
static void until(atomic_int* flag) {
    for (int i=0;i<8000;i++) {
        if (atomic_load_explicit(flag,memory_order_acquire)) return;
        const struct timespec pause={0,1000000};
        nanosleep(&pause,NULL);
    }
    fprintf(stderr,"RETAINED_ORDER timeout\n"); abort();
}
static void data(void* raw,size_t actor,uint64_t payload) {
    State* s=(State*)raw;
    CHECK(actor<2);
    CHECK(ores_native_actor_scheduler_is_current_actor(
        s->scheduler,s->actor[actor]));
    if (actor==1) {
        CHECK(payload==9);
        atomic_store_explicit(&s->other_progress,1,memory_order_release);
        return;
    }
    if (payload==1) {
        CHECK(ores_native_actor_scheduler_reserve_wake(
            s->scheduler,s->actor[0],s->actor[0],1,1,1,1,&s->wake));
        CHECK(ores_native_actor_scheduler_retain_current_data(
            s->scheduler,&s->retained));
        atomic_store_explicit(&s->first_suspended,1,memory_order_release);
    } else if (payload==2) {
        CHECK(atomic_load_explicit(&s->resumed,memory_order_acquire)==1);
        CHECK(ores_native_actor_scheduler_retained_data_count(
            s->scheduler,s->actor[0])==0);
        atomic_store_explicit(&s->later_ran,1,memory_order_release);
    } else CHECK(0);
}
static void wake_callback(void* raw,size_t actor,OresNativeWakeTicket ticket,
                          OresNativeWakeOutcome outcome) {
    State* s=(State*)raw;
    CHECK(actor==0 && outcome==ORES_NATIVE_WAKE_READY);
    CHECK(ores_native_actor_scheduler_is_current_wake(s->scheduler,ticket));
    CHECK(ores_native_actor_scheduler_is_current_actor(
        s->scheduler,s->actor[0]));
    CHECK(ticket.slot_generation==s->wake.slot_generation);
    CHECK(!ores_native_actor_scheduler_complete_retained_data(
        s->scheduler,(OresNativeRetainedDataTicket){0}));
    CHECK(ores_native_actor_scheduler_complete_retained_data(
        s->scheduler,s->retained));
    atomic_store_explicit(&s->resumed,1,memory_order_release);
}
int main(void) {
    State s={0};
    s.scheduler=ores_native_actor_scheduler_open_with_wakes(
        2,2,2,data,NULL,NULL,wake_callback,&s);
    CHECK(s.scheduler);
    s.actor[0]=ores_native_actor_scheduler_spawn(s.scheduler,0);
    s.actor[1]=ores_native_actor_scheduler_spawn(s.scheduler,1);
    CHECK(s.actor[0].scheduler_domain && s.actor[1].scheduler_domain);
    CHECK(ores_native_actor_scheduler_send(s.scheduler,s.actor[0],1));
    until(&s.first_suspended);
    CHECK(ores_native_actor_scheduler_send(s.scheduler,s.actor[0],2));
    CHECK(ores_native_actor_scheduler_try_send(s.scheduler,s.actor[0],3)
        == ORES_NATIVE_SEND_MAILBOX_FULL);
    CHECK(ores_native_actor_scheduler_send(s.scheduler,s.actor[1],9));
    until(&s.other_progress);
    CHECK(!atomic_load_explicit(&s.later_ran,memory_order_acquire));
    CHECK(!atomic_load_explicit(&s.resumed,memory_order_acquire));
    CHECK(ores_native_actor_scheduler_retained_data_count(
        s.scheduler,s.actor[0])==1);
    CHECK(!ores_native_actor_scheduler_complete_retained_data(
        s.scheduler,s.retained));
    CHECK(ores_native_actor_scheduler_transfer_wake(s.scheduler,s.wake));
    CHECK(ores_native_actor_scheduler_publish_wake(
        s.scheduler,s.wake,ORES_NATIVE_WAKE_READY)==ORES_NATIVE_WAKE_PUBLISHED);
    until(&s.resumed);
    until(&s.later_ran);
    CHECK(ores_native_actor_scheduler_drain(s.scheduler));
    CHECK(ores_native_actor_scheduler_accepted(s.scheduler)==4);
    CHECK(ores_native_actor_scheduler_completed(s.scheduler)==4);
    CHECK(ores_native_actor_scheduler_retained_data_count(
        s.scheduler,s.actor[0])==0);
    CHECK(ores_native_actor_scheduler_stop(s.scheduler,s.actor[0]));
    CHECK(ores_native_actor_scheduler_stop(s.scheduler,s.actor[1]));
    CHECK(ores_native_actor_scheduler_wait_done(s.scheduler,s.actor[0]));
    CHECK(ores_native_actor_scheduler_wait_done(s.scheduler,s.actor[1]));
    OresNativeActorTerminalState terminal={0};
    CHECK(ores_native_actor_scheduler_terminal_state(
        s.scheduler,s.actor[0],&terminal));
    CHECK(terminal.accepted_user_messages==2);
    CHECK(terminal.processed_user_messages==2);
    CHECK(terminal.abandoned_user_messages==0);
    ores_native_actor_scheduler_retire(s.scheduler);
    CHECK(ores_native_actor_scheduler_try_destroy(s.scheduler));
    puts("NATIVE_RETAINED_DATA_QUEUED_ORDER_PASS");
    return 0;
}
