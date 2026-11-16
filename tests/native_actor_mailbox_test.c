#define _POSIX_C_SOURCE 200809L
#include "oreslang/runtime/NativeActorMailbox.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#define THREADS 12
static void check(int okay) { if (!okay) abort(); }
static void nap(void) {
    struct timespec one_ms={0,1000000};
    nanosleep(&one_ms,NULL);
}
static void wait_for_pending(OresNativeActorMailbox* lane, size_t expected) {
    for (int i=0;i<2000;i++) {
        if (ores_native_actor_mailbox_pending(lane)==expected) return;
        nap();
    }
    check(0);
}
typedef struct {
    OresNativeActorMailbox* lane;
    atomic_int* active;
    atomic_int* max_active;
    atomic_int* finished;
    atomic_int* order_index;
    int* order;
    int id;
} ThreadArgs;
static void* worker(void* arg) {
    ThreadArgs* a=(ThreadArgs*)arg;
    if (!ores_native_actor_mailbox_enter(a->lane)) return NULL;
    int simultaneous=atomic_fetch_add(a->active,1)+1;
    int old=atomic_load(a->max_active);
    while (simultaneous>old
           && !atomic_compare_exchange_weak(a->max_active,&old,simultaneous)) { }
    if (a->order) {
        int index=atomic_fetch_add(a->order_index,1);
        check(index>=0 && index<THREADS);
        a->order[index]=a->id;
    }
    nap();
    atomic_fetch_sub(a->active,1);
    atomic_fetch_add(a->finished,1);
    ores_native_actor_mailbox_leave(a->lane);
    return NULL;
}
typedef struct {
    OresNativeActorMailbox* lane;
    atomic_int* result;
} TryArgs;
static void* try_worker(void* arg) {
    TryArgs* a=(TryArgs*)arg;
    int entered=ores_native_actor_mailbox_try_enter(a->lane);
    atomic_store(a->result,entered);
    if (entered) ores_native_actor_mailbox_leave(a->lane);
    return NULL;
}

/* Distinct actor-owned mailboxes must permit physically simultaneous turns.
 * This does not authorize concurrent entries on any one mailbox. */
typedef struct {
    OresNativeActorMailbox* lane;
    atomic_int* entered;
    atomic_int* active;
    atomic_int* peak;
    atomic_int* release;
} IndependentArgs;
static void* independent_worker(void* raw) {
    IndependentArgs* a = (IndependentArgs*)raw;
    if (!ores_native_actor_mailbox_enter(a->lane)) return (void*)(uintptr_t)1;
    int now = atomic_fetch_add(a->active, 1) + 1;
    int previous = atomic_load(a->peak);
    while (now > previous &&
           !atomic_compare_exchange_weak(a->peak, &previous, now)) { }
    atomic_fetch_add(a->entered, 1);
    while (!atomic_load(a->release)) nap();
    atomic_fetch_sub(a->active, 1);
    ores_native_actor_mailbox_leave(a->lane);
    return NULL;
}

int main(void) {
    /* Invalid capacities must not allocate an actor mailbox. */
    check(ores_native_actor_mailbox_open(0) == NULL);
    check(ores_native_actor_mailbox_open(65537) == NULL);
    OresNativeActorMailbox* lane=ores_native_actor_mailbox_open(THREADS+1);
    check(lane != NULL);
    atomic_int active=0, max_active=0, finished=0, order_index=0;
    int order[THREADS]={0};
    ThreadArgs args[THREADS];
    pthread_t threads[THREADS];
    /* Force exact queue order while lane owner is already admitted. */
    check(ores_native_actor_mailbox_enter(lane)==1);
    for(int i=0;i<THREADS;i++) {
        args[i]=(ThreadArgs){lane,&active,&max_active,&finished,&order_index,order,i};
        check(pthread_create(&threads[i],NULL,worker,&args[i])==0);
        wait_for_pending(lane,(size_t)i+2);
    }
    check(ores_native_actor_mailbox_pending(lane)==THREADS+1);
    ores_native_actor_mailbox_leave(lane);
    for(int i=0;i<THREADS;i++) check(pthread_join(threads[i],NULL)==0);
    check(atomic_load(&finished)==THREADS);
    check(atomic_load(&max_active)==1);
    check(atomic_load(&order_index)==THREADS);
    for(int i=0;i<THREADS;i++) check(order[i]==i);
    check(ores_native_actor_mailbox_pending(lane)==0);
    check(ores_native_actor_mailbox_rejected(lane)==0);
    ores_native_actor_mailbox_retire(lane);
    check(ores_native_actor_mailbox_enter(lane)==0);
    check(ores_native_actor_mailbox_try_destroy(lane)==1);

    OresNativeActorMailbox* bounded=ores_native_actor_mailbox_open(1);
    OresNativeActorMailbox* nested=ores_native_actor_mailbox_open(1);
    check(bounded != NULL && nested != NULL);
    check(ores_native_actor_mailbox_enter(bounded)==1);
    check(ores_native_actor_mailbox_enter(bounded)==0);
    check(ores_native_actor_mailbox_enter(nested)==0); /* no reentrancy */
    ThreadArgs denial={bounded,&active,&max_active,&finished,&order_index,NULL,0};
    pthread_t denied;
    check(pthread_create(&denied,NULL,worker,&denial)==0);
    check(pthread_join(denied,NULL)==0);
    check(ores_native_actor_mailbox_rejected(bounded)==1);
    check(ores_native_actor_mailbox_pending(bounded)==1);
    ores_native_actor_mailbox_retire(bounded);
    check(ores_native_actor_mailbox_try_destroy(bounded)==0);
    ores_native_actor_mailbox_leave(bounded);
    check(ores_native_actor_mailbox_try_destroy(bounded)==1);
    ores_native_actor_mailbox_retire(nested);
    check(ores_native_actor_mailbox_try_destroy(nested)==1);

    OresNativeActorMailbox* retiring=ores_native_actor_mailbox_open(2);
    check(retiring != NULL);
    check(ores_native_actor_mailbox_enter(retiring)==1);
    ThreadArgs waiting={retiring,&active,&max_active,&finished,&order_index,NULL,0};
    pthread_t blocked;
    check(pthread_create(&blocked,NULL,worker,&waiting)==0);
    wait_for_pending(retiring,2);
    ores_native_actor_mailbox_retire(retiring);
    check(ores_native_actor_mailbox_try_destroy(retiring)==0);
    ores_native_actor_mailbox_leave(retiring);
    check(pthread_join(blocked,NULL)==0);
    check(atomic_load(&finished)==THREADS);
    check(ores_native_actor_mailbox_pending(retiring)==0);
    check(ores_native_actor_mailbox_try_destroy(retiring)==1);
    /* Host nonparking try-admission cannot overtake a queued FIFO entrant. */
    OresNativeActorMailbox* probe=ores_native_actor_mailbox_open(2);
    check(probe != NULL);
    check(ores_native_actor_mailbox_try_enter(probe)==1);
    ThreadArgs queued={probe,&active,&max_active,&finished,&order_index,NULL,0};
    pthread_t queued_thread;
    check(pthread_create(&queued_thread,NULL,worker,&queued)==0);
    wait_for_pending(probe,2);
    atomic_int entered=0;
    TryArgs attempt={probe,&entered};
    pthread_t try_thread;
    check(pthread_create(&try_thread,NULL,try_worker,&attempt)==0);
    check(pthread_join(try_thread,NULL)==0);
    check(atomic_load(&entered)==0);
    check(ores_native_actor_mailbox_rejected(probe)==1);
    ores_native_actor_mailbox_leave(probe);
    check(pthread_join(queued_thread,NULL)==0);
    check(ores_native_actor_mailbox_pending(probe)==0);
    check(ores_native_actor_mailbox_try_enter(probe)==1);
    ores_native_actor_mailbox_leave(probe);
    ores_native_actor_mailbox_retire(probe);
    check(ores_native_actor_mailbox_try_enter(probe)==0);
    check(ores_native_actor_mailbox_try_destroy(probe)==1);

    OresNativeActorMailbox* isolated_a = ores_native_actor_mailbox_open(1);
    OresNativeActorMailbox* isolated_b = ores_native_actor_mailbox_open(1);
    check(isolated_a != NULL && isolated_b != NULL);
    atomic_int distinct_entered = 0, distinct_active = 0, distinct_peak = 0;
    atomic_int distinct_release = 0;
    IndependentArgs independent[2] = {
        {isolated_a, &distinct_entered, &distinct_active, &distinct_peak, &distinct_release},
        {isolated_b, &distinct_entered, &distinct_active, &distinct_peak, &distinct_release}
    };
    pthread_t carriers[2];
    for (int i = 0; i < 2; i++)
        check(pthread_create(&carriers[i], NULL, independent_worker, &independent[i]) == 0);
    int observed_two_live = 0;
    for (int i = 0; i < 5000; i++) {
        if (atomic_load(&distinct_entered) == 2) {
            observed_two_live = 1;
            break;
        }
        nap();
    }
    /* Release both carriers on timeout so the test never strands its threads. */
    atomic_store(&distinct_release, 1);
    for (int i = 0; i < 2; i++) {
        void* status = NULL;
        check(pthread_join(carriers[i], &status) == 0);
        check(status == NULL);
    }
    check(observed_two_live);
    check(atomic_load(&distinct_peak) == 2);
    check(atomic_load(&distinct_active) == 0);
    check(ores_native_actor_mailbox_pending(isolated_a) == 0);
    check(ores_native_actor_mailbox_pending(isolated_b) == 0);
    ores_native_actor_mailbox_retire(isolated_a);
    ores_native_actor_mailbox_retire(isolated_b);
    check(ores_native_actor_mailbox_try_destroy(isolated_a) == 1);
    check(ores_native_actor_mailbox_try_destroy(isolated_b) == 1);

    /* Retirement must unblock *all* pre-admitted waiters without allowing
     * any queued handler to start after the retired marker is set. Test the
     * maximum small FIFO backlog, not just a single blocked waiter. */
    OresNativeActorMailbox* retiring_many =
        ores_native_actor_mailbox_open(THREADS + 1);
    check(retiring_many != NULL);
    check(ores_native_actor_mailbox_enter(retiring_many) == 1);
    atomic_int abandoned_active = 0, abandoned_max = 0, abandoned_finished = 0;
    ThreadArgs abandoned[THREADS];
    pthread_t abandoned_threads[THREADS];
    for (int i = 0; i < THREADS; i++) {
        abandoned[i] = (ThreadArgs){retiring_many, &abandoned_active,
            &abandoned_max, &abandoned_finished, &order_index, NULL, i};
        check(pthread_create(&abandoned_threads[i], NULL, worker,
                &abandoned[i]) == 0);
        wait_for_pending(retiring_many, (size_t)i + 2);
    }
    check(ores_native_actor_mailbox_pending(retiring_many) == THREADS + 1);
    /* Already admitted waiters wake on retirement, return denial, and must
     * never run guest work or decrement the active owner's pending credit. */
    ores_native_actor_mailbox_retire(retiring_many);
    ores_native_actor_mailbox_retire(retiring_many); /* idempotent */
    check(ores_native_actor_mailbox_try_enter(retiring_many) == 0);
    check(ores_native_actor_mailbox_try_destroy(retiring_many) == 0);
    for (int i = 0; i < THREADS; i++)
        check(pthread_join(abandoned_threads[i], NULL) == 0);
    check(atomic_load(&abandoned_finished) == 0);
    check(atomic_load(&abandoned_active) == 0);
    check(atomic_load(&abandoned_max) == 0);
    check(ores_native_actor_mailbox_pending(retiring_many) == 1);
    ores_native_actor_mailbox_leave(retiring_many);
    check(ores_native_actor_mailbox_pending(retiring_many) == 0);
    check(ores_native_actor_mailbox_try_destroy(retiring_many) == 1);


    /* Graceful shutdown: queued tickets finish, late tickets do not enter. */
    OresNativeActorMailbox* draining=ores_native_actor_mailbox_open(3);
    check(draining != NULL);
    check(ores_native_actor_mailbox_enter(draining)==1);
    int delivered_before=atomic_load(&finished);
    ThreadArgs admitted={draining,&active,&max_active,&finished,&order_index,NULL,0};
    pthread_t admitted_thread;
    check(pthread_create(&admitted_thread,NULL,worker,&admitted)==0);
    wait_for_pending(draining,2);
    check(ores_native_actor_mailbox_begin_drain(draining)==1);
    check(ores_native_actor_mailbox_begin_drain(draining)==0);
    check(ores_native_actor_mailbox_try_destroy(draining)==0);
    atomic_int late_entered=1;
    TryArgs late={draining,&late_entered};
    pthread_t late_thread;
    check(pthread_create(&late_thread,NULL,try_worker,&late)==0);
    check(pthread_join(late_thread,NULL)==0);
    check(atomic_load(&late_entered)==0);
    check(ores_native_actor_mailbox_rejected(draining)==1);
    ores_native_actor_mailbox_leave(draining);
    check(pthread_join(admitted_thread,NULL)==0);
    check(atomic_load(&finished)==delivered_before+1);
    check(ores_native_actor_mailbox_pending(draining)==0);
    check(ores_native_actor_mailbox_try_enter(draining)==0);
    check(ores_native_actor_mailbox_try_destroy(draining)==1);

    /* An idle mailbox completes drain synchronously and cannot be revived. */
    OresNativeActorMailbox* idle=ores_native_actor_mailbox_open(1);
    check(idle != NULL);
    check(ores_native_actor_mailbox_begin_drain(idle)==1);
    check(ores_native_actor_mailbox_enter(idle)==0);
    check(ores_native_actor_mailbox_try_destroy(idle)==1);

    /* Abort overrides graceful drain, cancelling an already queued ticket. */
    OresNativeActorMailbox* aborted=ores_native_actor_mailbox_open(2);
    check(aborted != NULL);
    check(ores_native_actor_mailbox_enter(aborted)==1);
    int before_abort=atomic_load(&finished);
    ThreadArgs cancelled={aborted,&active,&max_active,&finished,&order_index,NULL,0};
    pthread_t cancelled_thread;
    check(pthread_create(&cancelled_thread,NULL,worker,&cancelled)==0);
    wait_for_pending(aborted,2);
    check(ores_native_actor_mailbox_begin_drain(aborted)==1);
    ores_native_actor_mailbox_retire(aborted);
    ores_native_actor_mailbox_leave(aborted);
    check(pthread_join(cancelled_thread,NULL)==0);
    check(atomic_load(&finished)==before_abort);
    check(ores_native_actor_mailbox_try_destroy(aborted)==1);

    puts("NATIVE_ACTOR_FIFO_MAILBOX_PASS fifo=12 max_active=1 distinct_mailbox_overlap=2 retire_waiters=12 capacity=1 retire_wake=1 try_admission=1 graceful_drain=1 abort_precedence=1");
    return 0;
}
