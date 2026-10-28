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
    atomic_int admitted, go, wake_count, cleanup_count, guest_active, errors;
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
    if (!ores_native_actor_scheduler_stop_sealed(f->scheduler,f->actor))
        atomic_fetch_add(&f->errors,1);
    return NULL;
}
static void* publisher(void* raw) {
    Fixture* f=(Fixture*)raw;
    wait_for(&f->go);
    if (ores_native_actor_scheduler_transfer_wake(
            f->scheduler,f->wake_ticket)) {
        atomic_store(&f->transfer_won,1);
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
    return NULL;
}
int main(void) {
    for (int i=0;i<80;++i) {
        Fixture f={0};
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
    puts("NATIVE_RETAINED_STOP_WAKE_RACE_PASS iterations=80");
    return 0;
}
