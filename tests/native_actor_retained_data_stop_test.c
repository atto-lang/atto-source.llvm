#define _POSIX_C_SOURCE 200809L
#include "oreslang/runtime/NativeActorScheduler.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, \
    "RETAINED_DATA_STOP_FAIL %s:%d: %s\n",__FILE__,__LINE__,#x); abort(); } } while (0)

typedef struct {
    OresNativeActorScheduler* scheduler;
    OresNativeActorRef actor;
    OresNativeRetainedDataTicket ticket;
    atomic_int data_entered, data_release, control_entered, control_release;
} State;
static void until(atomic_int* flag) {
    for (int i=0;i<8000;i++) {
        if (atomic_load_explicit(flag,memory_order_acquire)) return;
        const struct timespec pause={0,1000000};
        nanosleep(&pause,NULL);
    }
    fprintf(stderr,"RETAINED_DATA_STOP timeout\n"); abort();
}
static void data(void* raw,size_t actor,uint64_t payload) {
    State* s=(State*)raw;
    CHECK(actor==0 && payload==11);
    CHECK(ores_native_actor_scheduler_is_current_actor(s->scheduler,s->actor));
    CHECK(ores_native_actor_scheduler_retain_current_data(s->scheduler,&s->ticket));
    CHECK(!ores_native_actor_scheduler_retain_current_data(s->scheduler,&(OresNativeRetainedDataTicket){0}));
    CHECK(ores_native_actor_scheduler_retained_data_count(
        s->scheduler,s->actor)==1);
    atomic_store_explicit(&s->data_entered,1,memory_order_release);
    until(&s->data_release);
}
static void control(void* raw,size_t actor,uint64_t payload) {
    State* s=(State*)raw;
    CHECK(actor==0 && payload==31);
    CHECK(ores_native_actor_scheduler_is_current_actor(s->scheduler,s->actor));
    CHECK(ores_native_actor_scheduler_retained_data_count(
        s->scheduler,s->actor)==1);
    atomic_store_explicit(&s->control_entered,1,memory_order_release);
    until(&s->control_release);
    OresNativeRetainedDataTicket forged=s->ticket;
    forged.actor.generation++;
    CHECK(!ores_native_actor_scheduler_abandon_retained_data(
        s->scheduler,forged));
    CHECK(ores_native_actor_scheduler_abandon_retained_data(
        s->scheduler,s->ticket));
    CHECK(!ores_native_actor_scheduler_abandon_retained_data(
        s->scheduler,s->ticket));
    CHECK(!ores_native_actor_scheduler_complete_retained_data(
        s->scheduler,s->ticket));
}
int main(void) {
    State state={0};
    state.scheduler=ores_native_actor_scheduler_open_control(
        1,1,1,data,control,&state);
    CHECK(state.scheduler);
    state.actor=ores_native_actor_scheduler_spawn(state.scheduler,0);
    CHECK(state.actor.scheduler_domain);
    CHECK(!ores_native_actor_scheduler_retain_current_data(
        state.scheduler,&state.ticket));
    CHECK(ores_native_actor_scheduler_send(state.scheduler,state.actor,11));
    until(&state.data_entered);
    CHECK(ores_native_actor_scheduler_try_send(state.scheduler,state.actor,12)
        == ORES_NATIVE_SEND_MAILBOX_FULL);
    CHECK(!ores_native_actor_scheduler_complete_retained_data(
        state.scheduler,state.ticket));
    CHECK(ores_native_actor_scheduler_publish_control(
        state.scheduler,state.actor,31));
    CHECK(ores_native_actor_scheduler_stop_sealed(
        state.scheduler,state.actor));
    CHECK(!ores_native_actor_scheduler_poll_done(state.scheduler,state.actor));
    atomic_store_explicit(&state.data_release,1,memory_order_release);
    until(&state.control_entered);
    /* DATA host callback has returned, but the guest handler's retained
     * completion and bounded credit remain outstanding until CONTROL cleanup. */
    CHECK(ores_native_actor_scheduler_retained_data_count(
        state.scheduler,state.actor)==1);
    CHECK(!ores_native_actor_scheduler_poll_done(state.scheduler,state.actor));
    CHECK(ores_native_actor_scheduler_accepted(state.scheduler)==2);
    CHECK(!ores_native_actor_scheduler_complete_retained_data(
        state.scheduler,state.ticket));
    atomic_store_explicit(&state.control_release,1,memory_order_release);
    CHECK(ores_native_actor_scheduler_wait_done(state.scheduler,state.actor));
    CHECK(ores_native_actor_scheduler_retained_data_count(
        state.scheduler,state.actor)==0);
    OresNativeActorTerminalState terminal={0};
    CHECK(ores_native_actor_scheduler_terminal_state(
        state.scheduler,state.actor,&terminal));
    CHECK(terminal.kind==ORES_NATIVE_TERMINATION_STOPPED);
    CHECK(terminal.accepted_user_messages==1);
    CHECK(terminal.processed_user_messages==0);
    CHECK(terminal.abandoned_user_messages==1);
    CHECK(ores_native_actor_scheduler_accepted(state.scheduler)==2);
    CHECK(ores_native_actor_scheduler_completed(state.scheduler)==1);
    CHECK(ores_native_actor_scheduler_drain(state.scheduler));
    ores_native_actor_scheduler_retire(state.scheduler);
    CHECK(ores_native_actor_scheduler_try_destroy(state.scheduler));
    puts("NATIVE_RETAINED_DATA_STOP_FENCING_PASS");
    return 0;
}
