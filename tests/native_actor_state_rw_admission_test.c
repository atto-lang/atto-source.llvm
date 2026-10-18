#include "oreslang/runtime/OresActorStateRwAdmission.h"
#include <pthread.h>
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>

static ores_actor_state_rw state = ORES_ACTOR_STATE_RW_INIT(77u,9u);
static uint64_t protected_count;

static ores_actor_state_acquisition request_access(ores_std_access mode) {
    unsigned int effects = mode == ORES_STD_READ
        ? ORES_STATE_EFFECT_READ
        : ORES_STATE_EFFECT_READ | ORES_STATE_EFFECT_WRITE;
    ores_actor_state_acquisition r;
    do {
        r = ores_state_handle_request(&state,77u,9u,true,effects,effects,mode);
    } while (r.verification == ORES_STATE_ALLOWED &&
             r.admission.code == ORES_STD_SYNC_RETRY);
    assert(r.verification == ORES_STATE_ALLOWED);
    assert(r.admission.code == ORES_STD_SYNC_GRANTED ||
           r.admission.code == ORES_STD_SYNC_QUEUED);
    return r;
}
static void wait_start(ores_std_ticket t) {
    for (;;) {
        ores_std_sync_code code=ores_state_start(&state,77u,9u,t);
        if (code==ORES_STD_SYNC_GRANTED) return;
        assert(code==ORES_STD_SYNC_RETRY || code==ORES_STD_SYNC_QUEUED);
        sched_yield(); /* host test only; production actor gets wake tickets */
    }
}
static void end_release(ores_std_ticket t) {
    ores_std_sync_code code;
    do { code=ores_state_end(&state,77u,9u,t); }
    while (code==ORES_STD_SYNC_RETRY);
    assert(code==ORES_STD_SYNC_GRANTED);
    ores_std_sync_code suspend_check = ores_state_ready_to_suspend(&state,77u,9u,t);
    assert(suspend_check == ORES_STD_SYNC_EXECUTING ||
           suspend_check == ORES_STD_SYNC_RETRY);
    ores_std_sync_result r;
    do {r=ores_state_release(&state,77u,9u,t);}
    while(r.code==ORES_STD_SYNC_RETRY);
    assert(r.code==ORES_STD_SYNC_GRANTED);
}
static void *writer_worker(void* p) {
    (void)p;
    for(unsigned i=0;i<4000;++i) {
        ores_actor_state_acquisition r=request_access(ORES_STD_WRITE);
        wait_start(r.admission.ticket);
        ores_std_sync_code suspend_check =
            ores_state_ready_to_suspend(&state,77u,9u,r.admission.ticket);
        assert(suspend_check == ORES_STD_SYNC_EXECUTING ||
               suspend_check == ORES_STD_SYNC_RETRY);
        ++protected_count;
        end_release(r.admission.ticket);
    }
    return 0;
}
static void *reader_worker(void* p) {
    (void)p;
    for(unsigned i=0;i<4000;++i) {
        ores_actor_state_acquisition r=request_access(ORES_STD_READ);
        wait_start(r.admission.ticket);
        uint64_t snapshot=protected_count;
        (void)snapshot;
        end_release(r.admission.ticket);
    }
    return 0;
}
static void test_actor_effects(void) {
    ores_actor_state_rw a=ORES_ACTOR_STATE_RW_INIT(123u,2u);
    ores_actor_state_acquisition r;
    r=ores_state_handle_request(&a,122u,2u,true,ORES_STATE_EFFECT_READ,
        ORES_STATE_EFFECT_READ,ORES_STD_READ);
    assert(r.verification==ORES_STATE_WRONG_ACTOR);
    r=ores_state_handle_request(&a,123u,3u,true,ORES_STATE_EFFECT_READ,
        ORES_STATE_EFFECT_READ,ORES_STD_READ);
    assert(r.verification==ORES_STATE_WRONG_ACTOR);
    r=ores_state_handle_request(&a,123u,2u,false,ORES_STATE_EFFECT_READ,
        ORES_STATE_EFFECT_READ,ORES_STD_READ);
    assert(r.verification==ORES_STATE_NOT_CRITICAL);
    r=ores_state_handle_request(&a,123u,2u,true,ORES_STATE_EFFECT_READ,
        ORES_STATE_EFFECT_WRITE,ORES_STD_WRITE);
    assert(r.verification==ORES_STATE_EFFECT_DENIED);
    r=ores_state_handle_request(&a,123u,2u,true,ORES_STATE_EFFECT_READ,
        ORES_STATE_EFFECT_READ|ORES_STATE_EFFECT_IO,ORES_STD_READ);
    assert(r.verification==ORES_STATE_EFFECT_DENIED);
    r=ores_state_handle_request(&a,123u,2u,true,ORES_STATE_EFFECT_WRITE,
        ORES_STATE_EFFECT_READ|ORES_STATE_EFFECT_WRITE,ORES_STD_WRITE);
    assert(r.verification==ORES_STATE_EFFECT_DENIED);
    /* A declared write capability cannot justify a write-effect callback
     * while holding only a reader loan. Nor can a state-backed callback
     * suspend or cross an unknown FFI boundary while its loan is live. */
    r=ores_state_handle_request(&a,123u,2u,true,
        ORES_STATE_EFFECT_READ|ORES_STATE_EFFECT_WRITE,
        ORES_STATE_EFFECT_READ|ORES_STATE_EFFECT_WRITE,ORES_STD_READ);
    assert(r.verification==ORES_STATE_EFFECT_DENIED);
    r=ores_state_handle_request(&a,123u,2u,true,
        ORES_STATE_EFFECT_READ|ORES_STATE_EFFECT_SUSPEND,
        ORES_STATE_EFFECT_READ|ORES_STATE_EFFECT_SUSPEND,ORES_STD_READ);
    assert(r.verification==ORES_STATE_EFFECT_DENIED);
    r=ores_state_handle_request(&a,123u,2u,true,
        ORES_STATE_EFFECT_WRITE|ORES_STATE_EFFECT_FFI,
        ORES_STATE_EFFECT_WRITE|ORES_STATE_EFFECT_FFI,ORES_STD_WRITE);
    assert(r.verification==ORES_STATE_EFFECT_DENIED);
    r=ores_state_handle_request(&a,123u,2u,true,ORES_STATE_EFFECT_READ,
        ORES_STATE_EFFECT_READ,ORES_STD_READ);
    assert(r.verification==ORES_STATE_ALLOWED &&
           r.admission.code==ORES_STD_SYNC_GRANTED);
    assert(ores_state_start(&a,123u,3u,r.admission.ticket)==ORES_STD_SYNC_INVALID);
    assert(ores_state_ready_to_suspend(&a,123u,2u,r.admission.ticket)==ORES_STD_SYNC_EXECUTING);
    assert(ores_state_start(&a,123u,2u,r.admission.ticket)==ORES_STD_SYNC_GRANTED);
    assert(ores_state_release(&a,123u,2u,r.admission.ticket).code==ORES_STD_SYNC_EXECUTING);
    assert(ores_state_stop(&a,123u,3u).code==ORES_STD_SYNC_INVALID);
    ores_std_sync_result stop=ores_state_stop(&a,123u,2u);
    assert(stop.code==ORES_STD_SYNC_GRANTED && stop.failed_count==0);
    assert(ores_state_quiescent(&a,123u,2u)==ORES_STD_SYNC_EXECUTING);
    assert(ores_state_end(&a,123u,2u,r.admission.ticket)==ORES_STD_SYNC_GRANTED);
    assert(ores_state_release(&a,123u,2u,r.admission.ticket).code==ORES_STD_SYNC_GRANTED);
    assert(ores_state_quiescent(&a,123u,2u)==ORES_STD_SYNC_GRANTED);
    r=ores_state_handle_request(&a,123u,2u,true,ORES_STATE_EFFECT_READ,
        ORES_STATE_EFFECT_READ,ORES_STD_READ);
    assert(r.verification==ORES_STATE_ALLOWED &&
           r.admission.code==ORES_STD_SYNC_CLOSED);
}
int main(void) {
    test_actor_effects();
    pthread_t writers[3],readers[4];
    for(unsigned i=0;i<3;++i) assert(!pthread_create(&writers[i],0,writer_worker,0));
    for(unsigned i=0;i<4;++i) assert(!pthread_create(&readers[i],0,reader_worker,0));
    for(unsigned i=0;i<3;++i) assert(!pthread_join(writers[i],0));
    for(unsigned i=0;i<4;++i) assert(!pthread_join(readers[i],0));
    assert(protected_count==12000);
    assert(ores_state_stop(&state,77u,9u).code==ORES_STD_SYNC_GRANTED);
    assert(ores_state_quiescent(&state,77u,9u)==ORES_STD_SYNC_GRANTED);
    puts("native actor-state RW/effect admission PASS");
    return 0;
}
