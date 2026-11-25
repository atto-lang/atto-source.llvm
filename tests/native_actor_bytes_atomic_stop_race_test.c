#define _POSIX_C_SOURCE 200809L
#include "oreslang/runtime/NativeActorBytesSchedulerBinding.h"
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

/* Deterministic active-turn hold, plus truly concurrent stop/send contenders.
 * HOST TEST ONLY: the held callback is a test fixture, not a guest actor or
 * an approved zero-blocking-IO execution path. */
#define PRODUCERS 4
typedef struct {
    OresNativeActorScheduler* scheduler;
    OresNativeActorBytesSchedulerBinding bindings[2];
    atomic_int entered, release, primary_consumed, peer_consumed;
} Context;
typedef struct {
    Context* context;
    pthread_barrier_t* start;
    int status;
    int stopper;
    unsigned id;
} Contender;

static void check(int okay, const char* what) {
    if (!okay) { fprintf(stderr, "FAIL native_bytes_stop_race: %s\n", what); abort(); }
}
static void on_data(void* opaque, size_t index, uint64_t payload) {
    Context* ctx = (Context*)opaque;
    unsigned char out[8]={0};
    size_t n=SIZE_MAX;
    if (index == 0) {
        check(payload==1000, "actor0 unexpected extra DATA after sealed STOP");
        check(ores_native_actor_bytes_receive_current_data(
            &ctx->bindings[0], out, sizeof(out), &n)==ORES_ACTOR_BYTES_OK,
            "actor0 first physical DATA receipt");
        check(n==4 && out[0]==10 && out[1]==0 && out[2]==255 && out[3]==10,
            "actor0 exact owned payload");
        atomic_fetch_add_explicit(&ctx->primary_consumed,1,memory_order_release);
        atomic_store_explicit(&ctx->entered,1,memory_order_release);
        /* Deliberate host-only hold to make queued-vs-active STOP deterministic. */
        while (!atomic_load_explicit(&ctx->release,memory_order_acquire))
            sched_yield();
    } else if (index==1) {
        check(payload==2000, "peer unexpected DATA");
        check(ores_native_actor_bytes_receive_current_data(
            &ctx->bindings[1], out, sizeof(out), &n)==ORES_ACTOR_BYTES_OK,
            "peer physical DATA receipt");
        check(n==4 && out[0]==20 && out[1]==0 && out[2]==255 && out[3]==10,
            "peer untouched private bytes");
        atomic_fetch_add_explicit(&ctx->peer_consumed,1,memory_order_release);
    } else abort();
}
static void* race(void* opaque) {
    Contender* contender=(Contender*)opaque;
    int barrier=pthread_barrier_wait(contender->start);
    check(barrier==0 || barrier==PTHREAD_BARRIER_SERIAL_THREAD,"barrier");
    if (contender->stopper) {
        contender->status=ores_native_actor_scheduler_stop_sealed(
            contender->context->scheduler,
            contender->context->bindings[0].actor);
    } else {
        unsigned char bytes[4]={(unsigned char)(40+contender->id),0,255,10};
        contender->status=(int)ores_native_actor_bytes_try_send_atomic(
            &contender->context->bindings[0],
            1100+contender->id,bytes,sizeof(bytes));
    }
    return NULL;
}
static void observe(OresNativeActorBytesSchedulerBinding* binding,
    size_t want_count,size_t want_bytes) {
    size_t n=SIZE_MAX, bytes=SIZE_MAX;
    check(ores_native_actor_bytes_observe(binding->inbox,binding->storage,
          &n,&bytes)==ORES_ACTOR_BYTES_OK,"observe mailbox");
    check(n==want_count && bytes==want_bytes,"matching byte credits");
}
static void host_wait_for(atomic_int* ready) {
    struct timespec nap={0,1000000};
    for (int i=0;i<5000;++i) {
        if (atomic_load_explicit(ready,memory_order_acquire)) return;
        nanosleep(&nap,NULL); /* only host driver waits, never actor */
    }
    check(0,"physical actor callback timeout");
}
int main(void) {
    Context ctx={0};
    ctx.scheduler=ores_native_actor_scheduler_open(2,4,2,on_data,&ctx);
    check(ctx.scheduler!=NULL,"open scheduler");
    OresNativeActorBytesInbox* boxes[2]={0};
    for (size_t i=0;i<2;++i) {
        OresNativeActorRef actor=ores_native_actor_scheduler_spawn(ctx.scheduler,i);
        check(actor.scheduler_domain && actor.generation,"spawn actor");
        boxes[i]=ores_native_actor_bytes_open(i+1,actor.generation,4,16,64);
        check(boxes[i]!=NULL,"allocate private bytes");
        check(ores_native_actor_bytes_bind_scheduler(
            &ctx.bindings[i],ctx.scheduler,actor,boxes[i]),"atomic mailbox bind");
    }
    unsigned char first[4]={10,0,255,10};
    check(ores_native_actor_bytes_try_send_atomic(
        &ctx.bindings[0],1000,first,4)==ORES_NATIVE_SEND_ACCEPTED,
        "first message admitted");
    host_wait_for(&ctx.entered);
    check(atomic_load_explicit(&ctx.primary_consumed,memory_order_acquire)==1,
        "first data consumed before STOP");
    observe(&ctx.bindings[0],0,0);

    /* A payload rejection must not mint an ordinal, a DATA turn, or byte
     * credit. The next successful enqueue remains source-mail #2. */
    unsigned char too_large[17]={0};
    check(ores_native_actor_bytes_try_send_atomic(
        &ctx.bindings[0],1999,too_large,sizeof(too_large))
        ==ORES_NATIVE_SEND_RESOURCE_EXHAUSTED,
        "oversized payload refused atomically");
    observe(&ctx.bindings[0],0,0);
    unsigned char queued[4]={11,0,255,10};
    check(ores_native_actor_bytes_try_send_atomic(
        &ctx.bindings[0],1001,queued,4)==ORES_NATIVE_SEND_ACCEPTED,
        "second DATA queued while first actor active");
    observe(&ctx.bindings[0],1,4);

    /* Start exactly four send contenders and STOP simultaneously.
     * Only two additional DATA credits remain; whatever STOP admits
     * before linearization must be counted as abandoned (not processed),
     * and every post-STOP attempt must leave both queues unchanged. */
    pthread_barrier_t start;
    check(pthread_barrier_init(&start,NULL,PRODUCERS+1)==0,"barrier init");
    pthread_t threads[PRODUCERS+1];
    Contender inputs[PRODUCERS+1]={0};
    for (unsigned i=0;i<PRODUCERS+1;++i) {
        inputs[i]=(Contender){.context=&ctx,.start=&start,
            .status=-999,.stopper=(i==PRODUCERS),.id=i};
        check(pthread_create(&threads[i],NULL,race,&inputs[i])==0,"thread");
    }
    int additional=0, full=0, terminated=0;
    for (unsigned i=0;i<PRODUCERS+1;++i)
        check(pthread_join(threads[i],NULL)==0,"join");
    check(pthread_barrier_destroy(&start)==0,"barrier destroy");
    check(inputs[PRODUCERS].status==1,"STOP sealed exactly once");
    for (unsigned i=0;i<PRODUCERS;++i) {
        const int result=inputs[i].status;
        if (result==ORES_NATIVE_SEND_ACCEPTED) additional++;
        else if (result==ORES_NATIVE_SEND_MAILBOX_FULL) full++;
        else if (result==ORES_NATIVE_SEND_TERMINATED) terminated++;
        else check(0,"unexpected race admission status");
    }
    check(additional>=0 && additional<=2,"only two remaining credits");
    check(additional+full+terminated==PRODUCERS,"all race attempts classified");
    observe(&ctx.bindings[0],1+(size_t)additional,4*(1+(size_t)additional));

    unsigned char after_stop[4]={99,0,255,10};
    check(ores_native_actor_bytes_try_send_atomic(
        &ctx.bindings[0],1888,after_stop,4)==ORES_NATIVE_SEND_TERMINATED,
        "STOP rejects later Bytes");
    check(ores_native_actor_scheduler_try_send(
        ctx.scheduler,ctx.bindings[0].actor,1888)==ORES_NATIVE_SEND_TERMINATED,
        "STOP rejects scalar too");
    observe(&ctx.bindings[0],1+(size_t)additional,4*(1+(size_t)additional));
    check(ores_native_actor_bytes_try_destroy(
        boxes[0],ctx.bindings[0].storage)==ORES_ACTOR_BYTES_DENIED,
        "cannot physically free inbox before retire");

    /* A live peer still accepts private DATA while another actor stops. */
    unsigned char peer_data[4]={20,0,255,10};
    check(ores_native_actor_bytes_try_send_atomic(
        &ctx.bindings[1],2000,peer_data,4)==ORES_NATIVE_SEND_ACCEPTED,
        "peer survives STOP");
    atomic_store_explicit(&ctx.release,1,memory_order_release);
    check(ores_native_actor_scheduler_wait_done(
        ctx.scheduler,ctx.bindings[0].actor),"stopped actor DONE");
    host_wait_for(&ctx.peer_consumed);
    check(ores_native_actor_scheduler_stop(
        ctx.scheduler,ctx.bindings[1].actor),"peer stop");
    check(ores_native_actor_scheduler_wait_done(
        ctx.scheduler,ctx.bindings[1].actor),"peer DONE");

    OresNativeActorTerminalState stopped={0},peer={0};
    check(ores_native_actor_scheduler_terminal_state(
        ctx.scheduler,ctx.bindings[0].actor,&stopped),"terminal receipt");
    check(stopped.accepted_user_messages==(uint64_t)(2+additional),
        "all successful atomic sends accounted");
    check(stopped.processed_user_messages==1 &&
          stopped.abandoned_user_messages==(uint64_t)(1+additional),
        "every queued DATA explicitly abandoned on sealed STOP");
    check(stopped.accepted_user_messages ==
          stopped.processed_user_messages+stopped.abandoned_user_messages,
        "terminal conservation");
    check(ores_native_actor_scheduler_terminal_state(
        ctx.scheduler,ctx.bindings[1].actor,&peer),"peer terminal");
    check(peer.accepted_user_messages==1 &&
          peer.processed_user_messages==1 &&
          peer.abandoned_user_messages==0,"peer conservation");
    observe(&ctx.bindings[0],1+(size_t)additional,4*(1+(size_t)additional));
    observe(&ctx.bindings[1],0,0);
    for (size_t i=0;i<2;++i) {
        check(ores_native_actor_bytes_try_retire(
            boxes[i],ctx.bindings[i].storage)==ORES_ACTOR_BYTES_OK,
            "retirement zeroizes pending bytes");
        check(ores_native_actor_bytes_all_zero_for_test(boxes[i]),
            "full slab physically scrubbed");
        check(ores_native_actor_bytes_try_destroy(
            boxes[i],ctx.bindings[i].storage)==ORES_ACTOR_BYTES_OK,
            "destroy after physical actor DONE");
    }
    ores_native_actor_scheduler_retire(ctx.scheduler);
    check(ores_native_actor_scheduler_drain(ctx.scheduler),"scheduler drained");
    check(ores_native_actor_scheduler_try_destroy(ctx.scheduler),"scheduler freed");
    printf("NATIVE_ATOMIC_BYTES_STOP_RACE_HOST_ONLY_PASS producers=%d additional_accepted=%d mailbox_full=%d stop_won=%d accepted=%d processed=1 abandoned=%d peer=1 exact_conservation=1 scrubbed=1\n",
        PRODUCERS,additional,full,terminated,2+additional,1+additional);
    return 0;
}
