#include "oreslang/runtime/NativeActorMailbox.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#define THREADS 12
static void check(int okay) { if (!okay) abort(); }
typedef struct { OresNativeActorMailbox* lane; atomic_int* active; atomic_int* max_active; atomic_int* finished; } ThreadArgs;
static void* worker(void* arg) {
    ThreadArgs* a=(ThreadArgs*)arg;
    if (!ores_native_actor_mailbox_enter(a->lane)) return NULL;
    int simultaneous=atomic_fetch_add(a->active, 1)+1;
    int old=atomic_load(a->max_active);
    while (simultaneous>old && !atomic_compare_exchange_weak(a->max_active, &old, simultaneous)) { }
    struct timespec nap={0,1000000};
    nanosleep(&nap,NULL);
    atomic_fetch_sub(a->active,1);
    atomic_fetch_add(a->finished,1);
    ores_native_actor_mailbox_leave(a->lane);
    return NULL;
}
int main(void) {
    OresNativeActorMailbox* lane=ores_native_actor_mailbox_open(THREADS+1);
    check(lane != NULL);
    atomic_int active=0, max_active=0, finished=0;
    ThreadArgs arg={lane,&active,&max_active,&finished};
    pthread_t threads[THREADS];
    for(int i=0;i<THREADS;i++) check(pthread_create(&threads[i],NULL,worker,&arg)==0);
    for(int i=0;i<THREADS;i++) check(pthread_join(threads[i],NULL)==0);
    check(atomic_load(&finished)==THREADS);
    check(atomic_load(&max_active)==1);
    check(ores_native_actor_mailbox_rejected(lane)==0);
    ores_native_actor_mailbox_retire(lane);
    check(ores_native_actor_mailbox_enter(lane)==0);
    check(ores_native_actor_mailbox_try_destroy(lane)==1);

    OresNativeActorMailbox* bounded=ores_native_actor_mailbox_open(1);
    check(bounded != NULL);
    check(ores_native_actor_mailbox_enter(bounded)==1);
    check(ores_native_actor_mailbox_enter(bounded)==0); /* nested: no deadlock */
    check(ores_native_actor_mailbox_rejected(bounded)==0);
    ores_native_actor_mailbox_retire(bounded);
    check(ores_native_actor_mailbox_try_destroy(bounded)==0);
    ores_native_actor_mailbox_leave(bounded);
    check(ores_native_actor_mailbox_try_destroy(bounded)==1);
    puts("NATIVE_ACTOR_FIFO_MAILBOX_PASS");
    return 0;
}
