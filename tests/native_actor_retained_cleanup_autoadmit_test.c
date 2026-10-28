#define _POSIX_C_SOURCE 200809L
#include "oreslang/runtime/NativeActorScheduler.h"
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, \
    "NATIVE_PREPAID_CLEANUP_FAIL %s:%d: %s\n",__FILE__,__LINE__,#x); abort(); } } while (0)
typedef struct {
    OresNativeActorScheduler* scheduler;
    OresNativeActorRef actor;
    OresNativeRetainedDataTicket ticket;
    atomic_int entered, release_data, cleanup_entered, release_cleanup;
    atomic_int callbacks, cleanup_result;
} Fixture;
static void wait_for(atomic_int* ready) {
    for (int i=0;i<8000;++i) {
        if (atomic_load_explicit(ready,memory_order_acquire)) return;
        const struct timespec t={0,1000000};
        nanosleep(&t,NULL);
    }
    fprintf(stderr,"prepaid cleanup timeout\n"); abort();
}
static void data(void* raw,size_t actor,uint64_t value) {
    Fixture* f=(Fixture*)raw;
    CHECK(actor==0 && value==17);
    CHECK(ores_native_actor_scheduler_is_current_actor(f->scheduler,f->actor));
    CHECK(ores_native_actor_scheduler_retain_current_data(f->scheduler,&f->ticket));
    atomic_store_explicit(&f->entered,1,memory_order_release);
    wait_for(&f->release_data);
    /* The guest frame has suspended; its native callback may return even
     * after STOP, but its retained physical heap remains pinned. */
}
static int cleanup(void* raw,size_t actor,OresNativeRetainedDataTicket ticket) {
    Fixture* f=(Fixture*)raw;
    CHECK(actor==0);
    CHECK(ores_native_actor_scheduler_is_current_actor(f->scheduler,f->actor));
    CHECK(ticket.actor.scheduler_domain==f->actor.scheduler_domain);
    CHECK(ticket.actor.generation==f->actor.generation);
    CHECK(ticket.slot==f->ticket.slot);
    CHECK(ticket.slot_generation==f->ticket.slot_generation);
    CHECK(ticket.execution_id==f->ticket.execution_id);
    CHECK(ores_native_actor_scheduler_retained_data_count(
        f->scheduler,f->actor)==1);
    CHECK(!ores_native_actor_scheduler_retain_current_data(
        f->scheduler,&(OresNativeRetainedDataTicket){0}));
    atomic_fetch_add_explicit(&f->callbacks,1,memory_order_relaxed);
    atomic_store_explicit(&f->cleanup_entered,1,memory_order_release);
    wait_for(&f->release_cleanup);
    /* Host test simulates proven frame/borrow destruction. The scheduler
     * acknowledges abandon only AFTER this callback returns success. */
    return atomic_load_explicit(&f->cleanup_result,memory_order_acquire);
}
static void run_case(int cleanup_success) {
    Fixture f={0};
    atomic_store(&f.cleanup_result,cleanup_success);
    f.scheduler=ores_native_actor_scheduler_open(1,2,1,data,&f);
    CHECK(f.scheduler);
    CHECK(ores_native_actor_scheduler_configure_retained_cleanup(
        f.scheduler,cleanup));
    CHECK(!ores_native_actor_scheduler_configure_retained_cleanup(
        f.scheduler,cleanup));
    f.actor=ores_native_actor_scheduler_spawn(f.scheduler,0);
    CHECK(f.actor.scheduler_domain);
    CHECK(!ores_native_actor_scheduler_retained_cleanup_failed(
        f.scheduler,f.actor));
    CHECK(ores_native_actor_scheduler_send(f.scheduler,f.actor,17));
    wait_for(&f.entered);
    CHECK(!ores_native_actor_scheduler_poll_done(f.scheduler,f.actor));
    CHECK(ores_native_actor_scheduler_stop_sealed(f.scheduler,f.actor));
    CHECK(!ores_native_actor_scheduler_poll_done(f.scheduler,f.actor));
    CHECK(ores_native_actor_scheduler_retained_data_count(
        f.scheduler,f.actor)==1);
    atomic_store_explicit(&f.release_data,1,memory_order_release);
    wait_for(&f.cleanup_entered);
    CHECK(atomic_load(&f.callbacks)==1);
    CHECK(!ores_native_actor_scheduler_poll_done(f.scheduler,f.actor));
    CHECK(!ores_native_actor_scheduler_complete_retained_data(
        f.scheduler,f.ticket));
    CHECK(!ores_native_actor_scheduler_abandon_retained_data(
        f.scheduler,f.ticket));
    atomic_store_explicit(&f.release_cleanup,1,memory_order_release);
    if (cleanup_success == 1) {
        CHECK(ores_native_actor_scheduler_wait_done(f.scheduler,f.actor));
        CHECK(!ores_native_actor_scheduler_retained_cleanup_failed(
            f.scheduler,f.actor));
        CHECK(!ores_native_actor_scheduler_retained_data_count(
            f.scheduler,f.actor));
        OresNativeActorTerminalState terminal={0};
        CHECK(ores_native_actor_scheduler_terminal_state(
            f.scheduler,f.actor,&terminal));
        CHECK(terminal.kind==ORES_NATIVE_TERMINATION_STOPPED);
        CHECK(terminal.accepted_user_messages==1);
        CHECK(terminal.processed_user_messages==0);
        CHECK(terminal.abandoned_user_messages==1);
        CHECK(ores_native_actor_scheduler_accepted(f.scheduler)==2);
        CHECK(ores_native_actor_scheduler_completed(f.scheduler)==1);
        CHECK(ores_native_actor_scheduler_drain(f.scheduler));
        ores_native_actor_scheduler_retire(f.scheduler);
        CHECK(ores_native_actor_scheduler_try_destroy(f.scheduler));
    } else {
        CHECK(!ores_native_actor_scheduler_wait_done(f.scheduler,f.actor));
        CHECK(ores_native_actor_scheduler_retained_cleanup_failed(
            f.scheduler,f.actor));
        CHECK(!ores_native_actor_scheduler_poll_done(f.scheduler,f.actor));
        CHECK(ores_native_actor_scheduler_retained_data_count(
            f.scheduler,f.actor)==1);
        CHECK(!ores_native_actor_scheduler_drain(f.scheduler));
        ores_native_actor_scheduler_retire(f.scheduler);
        CHECK(!ores_native_actor_scheduler_try_destroy(f.scheduler));
        /* Failed physical cleanup remains quarantined and deliberately
         * cannot be destroyed; leak check is disabled for this subcase. */
    }
}
int main(void) {
    run_case(1);
#ifndef ORES_NATIVE_CLEANUP_SUCCESS_ONLY
    run_case(0);
    run_case(2); /* Noncanonical acknowledgement MUST also fail closed. */
#endif
    puts("NATIVE_PREPAID_STOP_CLEANUP_PASS");
    return 0;
}
