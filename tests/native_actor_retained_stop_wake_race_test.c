#define _POSIX_C_SOURCE 200809L
#include "oreslang/runtime/NativeActorScheduler.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, \
    "RETAINED_STOP_WAKE_RACE_FAIL %s:%d %s\n",__FILE__,__LINE__,#x); abort(); } } while (0)
typedef struct {
    OresNativeActorScheduler* scheduler;
    OresNativeActorRef actor;
    OresNativeWakeTicket wake_ticket;
    OresNativeRetainedDataTicket retained;
    int mode; /* 0 race, 1 STOP first, 2 publish first, 3 transfer-STOP-publish */
    atomic_int admitted, go, stop_done, publication_done;
    atomic_int wake_count, cleanup_count, guest_active, errors;
    atomic_int transfer_won, published, retired_publication, transfer_denied;
} Fixture;
static void wait_for(atomic_int* flag) {
    for (int i=0;i<8000;++i) {
        if (atomic_load_explicit(flag,memory_order_acquire)) return;
        const struct timespec t={0,1000000};
        nanosleep(&t,NULL);
    }
    fprintf(stderr,"retained STOP/wake race timeout\n"); abort();
}
static void data(void* raw,size_t index,uint64_t payload) {
    Fixture* f=(Fixture*)raw;
    CHECK(index==0 && payload==42);
    CHECK(ores_native_actor_scheduler_is_current_actor(f->scheduler,f->actor));
    CHECK(atomic_fetch_add_explicit(&f->guest_active,1,memory_order_acq_rel)==0);
    CHECK(ores_native_actor_scheduler_reserve_wake(f->scheduler,
        f->actor,f->actor,1,1,1,1,&f->wake_ticket));
    CHECK(ores_native_actor_scheduler_retain_current_data(
        f->scheduler,&f->retained));
    atomic_store_explicit(&f->admitted,1,memory_order_release);
    CHECK(atomic_fetch_sub_explicit(&f->guest_active,1,memory_order_acq_rel)==1);
}
static void wake(void* raw,size_t index,OresNativeWakeTicket t,
                 OresNativeWakeOutcome outcome) {
    Fixture* f=(Fixture*)raw;
    CHECK(index==0 && outcome==ORES_NATIVE_WAKE_READY);
    CHECK(ores_native_actor_scheduler_is_current_wake(f->scheduler,t));
    CHECK(ores_native_actor_scheduler_is_current_actor(f->scheduler,f->actor));
    CHECK(atomic_fetch_add_explicit(&f->guest_active,1,memory_order_acq_rel)==0);
    CHECK(ores_native_actor_scheduler_complete_retained_data(
        f->scheduler,f->retained));
    atomic_fetch_add_explicit(&f->wake_count,1,memory_order_relaxed);
    CHECK(atomic_fetch_sub_explicit(&f->guest_active,1,memory_order_acq_rel)==1);
}
static int cleanup(void* raw,size_t index,OresNativeRetainedDataTicket t) {
    Fixture* f=(Fixture*)raw;
    CHECK(index==0);
    CHECK(ores_native_actor_scheduler_is_current_actor(f->scheduler,f->actor));
    CHECK(t.actor.generation==f->retained.actor.generation);
    CHECK(t.slot_generation==f->retained.slot_generation);
    CHECK(t.execution_id==f->retained.execution_id);
    CHECK(atomic_fetch_add_explicit(&f->guest_active,1,memory_order_acq_rel)==0);
    atomic_fetch_add_explicit(&f->cleanup_count,1,memory_order_relaxed);
    CHECK(atomic_fetch_sub_explicit(&f->guest_active,1,memory_order_acq_rel)==1);
    return 1; /* test fixture: verified logical frame already unwound */
}
static void* stopper(void* raw) {
    Fixture* f=(Fixture*)raw;
    wait_for(&f->go);
    if (f->mode==2) wait_for(&f->publication_done);
    if (f->mode==3) wait_for(&f->transfer_won);
    if (!ores_native_actor_scheduler_stop_sealed(f->scheduler,f->actor))
        atomic_fetch_add(&f->errors,1);
    atomic_store_explicit(&f->stop_done,1,memory_order_release);
    return NULL;
}
static void* publisher(void* raw) {
    Fixture* f=(Fixture*)raw;
    wait_for(&f->go);
    if (f->mode==1) wait_for(&f->stop_done);
    if (ores_native_actor_scheduler_transfer_wake(
            f->scheduler,f->wake_ticket)) {
        atomic_store_explicit(&f->transfer_won,1,memory_order_release);
        if (f->mode==3) wait_for(&f->stop_done);
        OresNativeWakePublishStatus result=ores_native_actor_scheduler_publish_wake(
            f->scheduler,f->wake_ticket,ORES_NATIVE_WAKE_READY);
        if (result==ORES_NATIVE_WAKE_PUBLISHED) atomic_store(&f->published,1);
        else if (result==ORES_NATIVE_WAKE_PRODUCER_RETIRED)
            atomic_store(&f->retired_publication,1);
        else atomic_fetch_add(&f->errors,1);
    } else {
        atomic_store(&f->transfer_denied,1);
        if (!ores_native_actor_scheduler_cancel_wake(
                f->scheduler,f->wake_ticket))
            atomic_fetch_add(&f->errors,1);
    }
    atomic_store_explicit(&f->publication_done,1,memory_order_release);
    return NULL;
}

/* These two actors are deliberately distinct. The Future/producer runs on A;
 * target B can STOP independently after the credit was transferred. A must
 * receive an authenticated CANCELLED wake so its retained DATA can unwind. */
typedef struct {
    OresNativeActorScheduler* scheduler;
    OresNativeActorRef actors[2];
    OresNativeWakeTicket ticket;
    OresNativeRetainedDataTicket retained;
    atomic_int retained_ready, wake_count, cancelled;
} TargetStopFixture;
static void target_stop_data(void* raw,size_t actor,uint64_t payload) {
    TargetStopFixture* f=(TargetStopFixture*)raw;
    CHECK(actor==0 && payload==42);
    CHECK(ores_native_actor_scheduler_is_current_actor(
        f->scheduler,f->actors[0]));
    CHECK(ores_native_actor_scheduler_reserve_wake(
        f->scheduler,f->actors[0],f->actors[1],1,1,1,1,&f->ticket));
    CHECK(ores_native_actor_scheduler_retain_current_data(
        f->scheduler,&f->retained));
    atomic_store_explicit(&f->retained_ready,1,memory_order_release);
}
static void target_stop_wake(void* raw,size_t actor,OresNativeWakeTicket ticket,
                             OresNativeWakeOutcome outcome) {
    TargetStopFixture* f=(TargetStopFixture*)raw;
    CHECK(actor==0 && outcome==ORES_NATIVE_WAKE_CANCELLED);
    CHECK(ores_native_actor_scheduler_is_current_actor(
        f->scheduler,f->actors[0]));
    CHECK(ores_native_actor_scheduler_is_current_wake(f->scheduler,ticket));
    CHECK(ticket.slot_generation==f->ticket.slot_generation);
    CHECK(ticket.target.scheduler_domain==f->actors[1].scheduler_domain);
    CHECK(ticket.target.actor_slot==f->actors[1].actor_slot);
    CHECK(ticket.target.generation==f->actors[1].generation);
    CHECK(ores_native_actor_scheduler_complete_retained_data(
        f->scheduler,f->retained));
    CHECK(!ores_native_actor_scheduler_complete_retained_data(
        f->scheduler,f->retained));
    atomic_store_explicit(&f->cancelled,1,memory_order_release);
    atomic_fetch_add_explicit(&f->wake_count,1,memory_order_release);
}
static void test_target_stop_after_transfer(void) {
    TargetStopFixture f={0};
    f.scheduler=ores_native_actor_scheduler_open_with_wakes(
        2,2,2,target_stop_data,NULL,NULL,target_stop_wake,&f);
    CHECK(f.scheduler);
    f.actors[0]=ores_native_actor_scheduler_spawn(f.scheduler,0);
    f.actors[1]=ores_native_actor_scheduler_spawn(f.scheduler,1);
    CHECK(f.actors[0].scheduler_domain && f.actors[1].scheduler_domain);
    CHECK(ores_native_actor_scheduler_send(f.scheduler,f.actors[0],42));
    wait_for(&f.retained_ready);
    CHECK(ores_native_actor_scheduler_transfer_wake(f.scheduler,f.ticket));
    CHECK(ores_native_actor_scheduler_stop_sealed(f.scheduler,f.actors[1]));
    CHECK(ores_native_actor_scheduler_wait_done(f.scheduler,f.actors[1]));
    CHECK(ores_native_actor_scheduler_publish_wake(
        f.scheduler,f.ticket,ORES_NATIVE_WAKE_READY)
        == ORES_NATIVE_WAKE_TARGET_RETIRED);
    /* This is an internal CANCELLED callback on A, not a receipt for B. */
    CHECK(!ores_native_actor_scheduler_abort_transferred_wake(
        f.scheduler,f.ticket));
    CHECK(!ores_native_actor_scheduler_transfer_wake(f.scheduler,f.ticket));
    CHECK(ores_native_actor_scheduler_publish_wake(
        f.scheduler,f.ticket,ORES_NATIVE_WAKE_READY)
        == ORES_NATIVE_WAKE_INVALID);
    wait_for(&f.cancelled);
    CHECK(ores_native_actor_scheduler_drain(f.scheduler));
    CHECK(atomic_load_explicit(&f.wake_count,memory_order_acquire)==1);
    CHECK(ores_native_actor_scheduler_outstanding_wake_loans(
        f.scheduler,f.actors[0])==0);
    CHECK(ores_native_actor_scheduler_retained_data_count(
        f.scheduler,f.actors[0])==0);
    CHECK(ores_native_actor_scheduler_accepted(f.scheduler)==2);
    CHECK(ores_native_actor_scheduler_completed(f.scheduler)==2);
    CHECK(ores_native_actor_scheduler_wakes_completed(f.scheduler)==1);
    OresNativeActorTerminalState terminal={0};
    CHECK(ores_native_actor_scheduler_terminal_state(
        f.scheduler,f.actors[1],&terminal));
    CHECK(terminal.accepted_user_messages==0);
    CHECK(terminal.processed_user_messages==0);
    CHECK(terminal.abandoned_user_messages==0);
    CHECK(ores_native_actor_scheduler_stop_sealed(f.scheduler,f.actors[0]));
    CHECK(ores_native_actor_scheduler_wait_done(f.scheduler,f.actors[0]));
    ores_native_actor_scheduler_retire(f.scheduler);
    CHECK(ores_native_actor_scheduler_try_destroy(f.scheduler));
}
static void test_target_stop_before_transfer(void) {
    TargetStopFixture f={0};
    f.scheduler=ores_native_actor_scheduler_open_with_wakes(
        2,2,1,target_stop_data,NULL,NULL,target_stop_wake,&f);
    CHECK(f.scheduler);
    f.actors[0]=ores_native_actor_scheduler_spawn(f.scheduler,0);
    f.actors[1]=ores_native_actor_scheduler_spawn(f.scheduler,1);
    CHECK(f.actors[0].scheduler_domain && f.actors[1].scheduler_domain);
    CHECK(ores_native_actor_scheduler_reserve_wake(
        f.scheduler,f.actors[0],f.actors[1],1,1,1,1,&f.ticket));
    CHECK(ores_native_actor_scheduler_stop_sealed(f.scheduler,f.actors[1]));
    CHECK(ores_native_actor_scheduler_wait_done(f.scheduler,f.actors[1]));
    CHECK(!ores_native_actor_scheduler_transfer_wake(f.scheduler,f.ticket));
    CHECK(ores_native_actor_scheduler_cancel_wake(f.scheduler,f.ticket));
    CHECK(!ores_native_actor_scheduler_cancel_wake(f.scheduler,f.ticket));
    CHECK(ores_native_actor_scheduler_outstanding_wake_loans(
        f.scheduler,f.actors[0])==0);
    CHECK(ores_native_actor_scheduler_accepted(f.scheduler)==0);
    CHECK(ores_native_actor_scheduler_wakes_completed(f.scheduler)==0);
    CHECK(atomic_load(&f.wake_count)==0);
    CHECK(ores_native_actor_scheduler_stop_sealed(f.scheduler,f.actors[0]));
    CHECK(ores_native_actor_scheduler_wait_done(f.scheduler,f.actors[0]));
    CHECK(ores_native_actor_scheduler_drain(f.scheduler));
    ores_native_actor_scheduler_retire(f.scheduler);
    CHECK(ores_native_actor_scheduler_try_destroy(f.scheduler));
}
static void test_producer_stop_after_transfer(void) {
    TargetStopFixture f={0};
    f.scheduler=ores_native_actor_scheduler_open_with_wakes(
        2,2,1,target_stop_data,NULL,NULL,target_stop_wake,&f);
    CHECK(f.scheduler);
    f.actors[0]=ores_native_actor_scheduler_spawn(f.scheduler,0);
    f.actors[1]=ores_native_actor_scheduler_spawn(f.scheduler,1);
    CHECK(ores_native_actor_scheduler_reserve_wake(
        f.scheduler,f.actors[0],f.actors[1],1,1,1,1,&f.ticket));
    CHECK(ores_native_actor_scheduler_transfer_wake(f.scheduler,f.ticket));
    CHECK(ores_native_actor_scheduler_stop_sealed(f.scheduler,f.actors[0]));
    CHECK(ores_native_actor_scheduler_publish_wake(
        f.scheduler,f.ticket,ORES_NATIVE_WAKE_READY)
        == ORES_NATIVE_WAKE_PRODUCER_RETIRED);
    CHECK(ores_native_actor_scheduler_wait_done(f.scheduler,f.actors[0]));
    CHECK(ores_native_actor_scheduler_outstanding_wake_loans(
        f.scheduler,f.actors[0])==0);
    CHECK(ores_native_actor_scheduler_accepted(f.scheduler)==0);
    CHECK(atomic_load(&f.wake_count)==0);
    CHECK(ores_native_actor_scheduler_stop_sealed(f.scheduler,f.actors[1]));
    CHECK(ores_native_actor_scheduler_wait_done(f.scheduler,f.actors[1]));
    CHECK(ores_native_actor_scheduler_drain(f.scheduler));
    ores_native_actor_scheduler_retire(f.scheduler);
    CHECK(ores_native_actor_scheduler_try_destroy(f.scheduler));
}

int main(void) {
    for (int i=0;i<80;++i) {
        Fixture f={0};
        f.mode=i%4;
        f.scheduler=ores_native_actor_scheduler_open_with_wakes(
            1,2,1,data,NULL,NULL,wake,&f);
        CHECK(f.scheduler);
        CHECK(ores_native_actor_scheduler_configure_retained_cleanup(
            f.scheduler,cleanup));
        f.actor=ores_native_actor_scheduler_spawn(f.scheduler,0);
        CHECK(f.actor.scheduler_domain);
        CHECK(ores_native_actor_scheduler_send(f.scheduler,f.actor,42));
        wait_for(&f.admitted);
        pthread_t stop_thread,publish_thread;
        CHECK(pthread_create(&stop_thread,NULL,stopper,&f)==0);
        CHECK(pthread_create(&publish_thread,NULL,publisher,&f)==0);
        atomic_store_explicit(&f.go,1,memory_order_release);
        CHECK(pthread_join(stop_thread,NULL)==0);
        CHECK(pthread_join(publish_thread,NULL)==0);
        CHECK(!atomic_load(&f.errors));
        CHECK(atomic_load(&f.published)+atomic_load(&f.retired_publication)
            +atomic_load(&f.transfer_denied)==1);
        if (f.mode==1) CHECK(atomic_load(&f.transfer_denied)==1);
        if (f.mode==2) CHECK(atomic_load(&f.published)==1);
        if (f.mode==3) CHECK(atomic_load(&f.retired_publication)==1);
        CHECK(ores_native_actor_scheduler_wait_done(f.scheduler,f.actor));
        CHECK(ores_native_actor_scheduler_drain(f.scheduler));
        CHECK(atomic_load(&f.guest_active)==0);
        CHECK(atomic_load(&f.wake_count)+atomic_load(&f.cleanup_count)==1);
        CHECK(ores_native_actor_scheduler_retained_data_count(
            f.scheduler,f.actor)==0);
        OresNativeActorTerminalState terminal={0};
        CHECK(ores_native_actor_scheduler_terminal_state(
            f.scheduler,f.actor,&terminal));
        CHECK(terminal.kind==ORES_NATIVE_TERMINATION_STOPPED);
        CHECK(ores_native_actor_scheduler_control_completed(f.scheduler)==0);
        CHECK(terminal.accepted_user_messages==1);
        CHECK(terminal.processed_user_messages+terminal.abandoned_user_messages==1);
        CHECK(terminal.processed_user_messages==(uint64_t)atomic_load(&f.wake_count));
        CHECK(terminal.abandoned_user_messages==(uint64_t)atomic_load(&f.cleanup_count));
        CHECK(!ores_native_actor_scheduler_complete_retained_data(
            f.scheduler,f.retained));
        CHECK(!ores_native_actor_scheduler_abandon_retained_data(
            f.scheduler,f.retained));
        ores_native_actor_scheduler_retire(f.scheduler);
        CHECK(ores_native_actor_scheduler_try_destroy(f.scheduler));
    }
    test_target_stop_before_transfer();
    test_target_stop_after_transfer();
    test_producer_stop_after_transfer();
    puts("NATIVE_RETAINED_STOP_WAKE_RACE_PASS iterations=80 orderings=4 target_cases=3");
    return 0;
}
