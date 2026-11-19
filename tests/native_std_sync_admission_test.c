#include "oreslang/runtime/OresStdSyncAdmission.h"
#include <pthread.h>
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <sched.h>
#include <stdio.h>
#include <stdint.h>

static void wait_until_started(ores_std_sync_gate* gate, ores_std_ticket ticket) {
    for (;;) {
        ores_std_sync_code rc = ores_std_start(gate, ticket);
        if (rc == ORES_STD_SYNC_GRANTED) return;
        assert(rc == ORES_STD_SYNC_RETRY || rc == ORES_STD_SYNC_QUEUED);
        sched_yield(); /* host test only; actor scheduler will enqueue wakes */
    }
}
static ores_std_sync_result request_retry(
        ores_std_sync_gate* gate, ores_std_access access) {
    ores_std_sync_result r;
    do { r = ores_std_request(gate, access); } while (r.code == ORES_STD_SYNC_RETRY);
    assert(r.code == ORES_STD_SYNC_GRANTED || r.code == ORES_STD_SYNC_QUEUED);
    return r;
}
static ores_std_sync_result release_retry(
        ores_std_sync_gate* gate, ores_std_ticket t) {
    ores_std_sync_result r;
    do { r = ores_std_release(gate, t); } while (r.code == ORES_STD_SYNC_RETRY);
    return r;
}
static void end_retry(ores_std_sync_gate* gate, ores_std_ticket t) {
    ores_std_sync_code rc;
    do { rc = ores_std_end(gate, t); } while (rc == ORES_STD_SYNC_RETRY);
    assert(rc == ORES_STD_SYNC_GRANTED);
}

static void test_scope_reincarnation(void) {
    ores_std_sync_gate gate = ORES_STD_MUTEX_INIT;
    assert(ores_std_request(&gate, ORES_STD_WRITE).code == ORES_STD_SYNC_INVALID);
    assert(ores_std_bind_scope(&gate, 1001u) == ORES_STD_SYNC_GRANTED);
    assert(ores_std_bind_scope(&gate, 1002u) == ORES_STD_SYNC_INVALID);
    ores_std_sync_result a=ores_std_request(&gate, ORES_STD_WRITE);
    assert(a.code == ORES_STD_SYNC_GRANTED);
    assert(ores_std_release(&gate,a.ticket).code == ORES_STD_SYNC_GRANTED);
    assert(ores_std_seal(&gate).code == ORES_STD_SYNC_GRANTED);
    assert(ores_std_quiescent(&gate) == ORES_STD_SYNC_GRANTED);
    /* Trusted reuse only AFTER completed teardown and all callbacks stopped.
     * Recreate a new object incarnation at the same storage address. */
    gate.scope_generation = 2002u;
    gate.next_sequence = 0;
    gate.sealed = false;
    gate.exhausted = false;
    ores_std_sync_result b=ores_std_request(&gate, ORES_STD_WRITE);
    assert(b.code == ORES_STD_SYNC_GRANTED);
    assert(b.ticket.sequence == a.ticket.sequence);
    assert(b.ticket.scope == a.ticket.scope);
    assert(b.ticket.scope_generation != a.ticket.scope_generation);
    assert(ores_std_start(&gate,a.ticket) == ORES_STD_SYNC_STALE);
    assert(ores_std_release(&gate,a.ticket).code == ORES_STD_SYNC_STALE);
    assert(ores_std_release(&gate,b.ticket).code == ORES_STD_SYNC_GRANTED);
}
static void test_mutex_fifo_and_cancellation(void) {
    ores_std_sync_gate gate = ORES_STD_MUTEX_INIT_SCOPE(1u);
    ores_std_sync_result a = ores_std_request(&gate, ORES_STD_WRITE);
    ores_std_sync_result b = ores_std_request(&gate, ORES_STD_WRITE);
    ores_std_sync_result c = ores_std_request(&gate, ORES_STD_WRITE);
    ores_std_sync_result d = ores_std_request(&gate, ORES_STD_WRITE);
    assert(a.code == ORES_STD_SYNC_GRANTED);
    assert(b.code == ORES_STD_SYNC_QUEUED);
    assert(c.code == ORES_STD_SYNC_QUEUED);
    assert(d.code == ORES_STD_SYNC_QUEUED);
    assert(ores_std_start(&gate,a.ticket) == ORES_STD_SYNC_GRANTED);
    assert(ores_std_release(&gate,a.ticket).code == ORES_STD_SYNC_EXECUTING);
    assert(ores_std_release(&gate,c.ticket).code == ORES_STD_SYNC_GRANTED);
    assert(ores_std_start(&gate,c.ticket) == ORES_STD_SYNC_STALE);
    end_retry(&gate,a.ticket);
    ores_std_sync_result woke = ores_std_release(&gate,a.ticket);
    assert(woke.code == ORES_STD_SYNC_GRANTED);
    assert(woke.admitted_count == 1 && woke.admitted[0].sequence == b.ticket.sequence);
    assert(ores_std_start(&gate,b.ticket) == ORES_STD_SYNC_GRANTED);
    end_retry(&gate,b.ticket);
    woke = ores_std_release(&gate,b.ticket);
    assert(woke.admitted_count == 1 && woke.admitted[0].sequence == d.ticket.sequence);
    assert(ores_std_start(&gate,d.ticket) == ORES_STD_SYNC_GRANTED);
    end_retry(&gate,d.ticket);
    woke = ores_std_release(&gate,d.ticket);
    assert(woke.admitted_count == 0);
    assert(ores_std_release(&gate,d.ticket).code == ORES_STD_SYNC_STALE);
    ores_std_sync_gate foreign = ORES_STD_MUTEX_INIT_SCOPE(1u);
    ores_std_sync_result x = ores_std_request(&foreign, ORES_STD_WRITE);
    assert(ores_std_start(&gate,x.ticket) == ORES_STD_SYNC_STALE);
    assert(ores_std_release(&gate,x.ticket).code == ORES_STD_SYNC_STALE);
    assert(ores_std_release(&foreign,x.ticket).code == ORES_STD_SYNC_GRANTED);
}

static void test_rw_fairness_and_wake_cohorts(void) {
    ores_std_sync_gate gate = ORES_STD_RW_INIT_SCOPE(1u);
    ores_std_sync_result a = ores_std_request(&gate,ORES_STD_READ);
    ores_std_sync_result b = ores_std_request(&gate,ORES_STD_READ);
    ores_std_sync_result w = ores_std_request(&gate,ORES_STD_WRITE);
    ores_std_sync_result c = ores_std_request(&gate,ORES_STD_READ);
    ores_std_sync_result d = ores_std_request(&gate,ORES_STD_READ);
    assert(a.code == ORES_STD_SYNC_GRANTED);
    assert(b.code == ORES_STD_SYNC_GRANTED);
    assert(w.code == ORES_STD_SYNC_QUEUED);
    assert(c.code == ORES_STD_SYNC_QUEUED && d.code == ORES_STD_SYNC_QUEUED);
    assert(ores_std_start(&gate,a.ticket) == ORES_STD_SYNC_GRANTED);
    assert(ores_std_start(&gate,b.ticket) == ORES_STD_SYNC_GRANTED);
    end_retry(&gate,a.ticket);
    ores_std_sync_result out = ores_std_release(&gate,a.ticket);
    assert(out.admitted_count == 0);
    end_retry(&gate,b.ticket);
    out = ores_std_release(&gate,b.ticket);
    assert(out.admitted_count == 1 && out.admitted[0].sequence == w.ticket.sequence);
    assert(ores_std_start(&gate,c.ticket) == ORES_STD_SYNC_QUEUED);
    assert(ores_std_start(&gate,w.ticket) == ORES_STD_SYNC_GRANTED);
    assert(ores_std_release(&gate,w.ticket).code == ORES_STD_SYNC_EXECUTING);
    end_retry(&gate,w.ticket);
    out = ores_std_release(&gate,w.ticket);
    assert(out.admitted_count == 2);
    assert(out.admitted[0].sequence == c.ticket.sequence);
    assert(out.admitted[1].sequence == d.ticket.sequence);
    assert(ores_std_start(&gate,c.ticket) == ORES_STD_SYNC_GRANTED);
    assert(ores_std_start(&gate,d.ticket) == ORES_STD_SYNC_GRANTED);
    end_retry(&gate,c.ticket); end_retry(&gate,d.ticket);
    assert(ores_std_release(&gate,c.ticket).code == ORES_STD_SYNC_GRANTED);
    assert(ores_std_release(&gate,d.ticket).code == ORES_STD_SYNC_GRANTED);
}

static void test_seal_and_physical_quiescence(void) {
    ores_std_sync_gate gate = ORES_STD_MUTEX_INIT_SCOPE(1u);
    ores_std_sync_result active = ores_std_request(&gate, ORES_STD_WRITE);
    ores_std_sync_result queued = ores_std_request(&gate, ORES_STD_WRITE);
    assert(active.code == ORES_STD_SYNC_GRANTED);
    assert(queued.code == ORES_STD_SYNC_QUEUED);
    assert(ores_std_start(&gate, active.ticket) == ORES_STD_SYNC_GRANTED);
    ores_std_sync_result stop = ores_std_seal(&gate);
    assert(stop.code == ORES_STD_SYNC_GRANTED);
    assert(stop.failed_count == 1 && stop.failed[0].sequence == queued.ticket.sequence);
    assert(ores_std_request(&gate, ORES_STD_WRITE).code == ORES_STD_SYNC_CLOSED);
    assert(ores_std_start(&gate, queued.ticket) == ORES_STD_SYNC_STALE);
    assert(ores_std_seal(&gate).code == ORES_STD_SYNC_CLOSED);
    assert(ores_std_quiescent(&gate) == ORES_STD_SYNC_EXECUTING);
    assert(ores_std_release(&gate, active.ticket).code == ORES_STD_SYNC_EXECUTING);
    assert(ores_std_ready_to_suspend(&gate, active.ticket) == ORES_STD_SYNC_EXECUTING);
    end_retry(&gate, active.ticket);
    assert(ores_std_ready_to_suspend(&gate, active.ticket) == ORES_STD_SYNC_EXECUTING);
    assert(ores_std_release(&gate, active.ticket).code == ORES_STD_SYNC_GRANTED);
    assert(ores_std_quiescent(&gate) == ORES_STD_SYNC_GRANTED);
    assert(ores_std_ready_to_suspend(&gate, active.ticket) == ORES_STD_SYNC_GRANTED);

    ores_std_sync_gate rw = ORES_STD_RW_INIT_SCOPE(1u);
    ores_std_sync_result r1=ores_std_request(&rw, ORES_STD_READ);
    ores_std_sync_result r2=ores_std_request(&rw, ORES_STD_READ);
    ores_std_sync_result w=ores_std_request(&rw, ORES_STD_WRITE);
    assert(ores_std_start(&rw, r1.ticket) == ORES_STD_SYNC_GRANTED);
    stop = ores_std_seal(&rw);
    assert(stop.code == ORES_STD_SYNC_GRANTED);
    assert(stop.failed_count == 2);
    assert(ores_std_start(&rw, r2.ticket) == ORES_STD_SYNC_STALE);
    assert(ores_std_start(&rw, w.ticket) == ORES_STD_SYNC_STALE);
    end_retry(&rw, r1.ticket);
    assert(ores_std_release(&rw, r1.ticket).code == ORES_STD_SYNC_GRANTED);
    assert(ores_std_quiescent(&rw) == ORES_STD_SYNC_GRANTED);
}

typedef struct stop_race {
    ores_std_sync_gate gate;
    ores_std_ticket ticket;
    atomic_bool start;
    ores_std_sync_code admitted;
} stop_race;
static void *start_during_stop(void* payload) {
    stop_race* race=(stop_race*)payload;
    while(!atomic_load_explicit(&race->start,memory_order_acquire)) sched_yield();
    race->admitted=ores_std_start(&race->gate,race->ticket);
    return NULL;
}
static void test_closed_start_does_not_reactivate_dormant_grant(void) {
    ores_std_sync_gate gate = ORES_STD_MUTEX_INIT_SCOPE(500u);
    ores_std_sync_result admitted = ores_std_request(&gate, ORES_STD_WRITE);
    assert(admitted.code == ORES_STD_SYNC_GRANTED);
    /* Reproduce the linearization of STOP against a ticket that has been
     * admitted but has not begun a guest callback. */
    gate.sealed = true;
    assert(ores_std_start(&gate, admitted.ticket) == ORES_STD_SYNC_CLOSED);
    assert(ores_std_start(&gate, admitted.ticket) == ORES_STD_SYNC_CLOSED);
    assert(gate.slots[0].state == ORES_STD_ADMITTED);
    assert(ores_std_release(&gate, admitted.ticket).code == ORES_STD_SYNC_GRANTED);
    assert(ores_std_quiescent(&gate) == ORES_STD_SYNC_GRANTED);
}

static void test_stop_vs_start_race(void) {
    for(int i=0;i<500;++i) {
        stop_race race={ORES_STD_MUTEX_INIT_SCOPE(1u),{0},ATOMIC_VAR_INIT(false),ORES_STD_SYNC_INVALID};
        ores_std_sync_result r=ores_std_request(&race.gate,ORES_STD_WRITE);
        assert(r.code == ORES_STD_SYNC_GRANTED);
        race.ticket=r.ticket;
        pthread_t thread;
        assert(!pthread_create(&thread,NULL,start_during_stop,&race));
        atomic_store_explicit(&race.start,true,memory_order_release);
        ores_std_sync_result stop;
        do { stop=ores_std_seal(&race.gate); }
        while(stop.code == ORES_STD_SYNC_RETRY);
        assert(!pthread_join(thread,NULL));
        assert(race.admitted == ORES_STD_SYNC_GRANTED ||
               race.admitted == ORES_STD_SYNC_STALE ||
               race.admitted == ORES_STD_SYNC_CLOSED ||
               race.admitted == ORES_STD_SYNC_RETRY);
        if(race.admitted == ORES_STD_SYNC_GRANTED) {
            end_retry(&race.gate,race.ticket);
            assert(release_retry(&race.gate,race.ticket).code == ORES_STD_SYNC_GRANTED);
        } else {
            assert(stop.failed_count == 1);
        }
        assert(ores_std_quiescent(&race.gate) == ORES_STD_SYNC_GRANTED);
    }
}

static ores_std_sync_gate mutex = ORES_STD_MUTEX_INIT_SCOPE(1u);
static ores_std_sync_gate rw = ORES_STD_RW_INIT_SCOPE(1u);
static uint64_t mutex_counter, writer_counter;
static atomic_uint overlap = ATOMIC_VAR_INIT(0u);

static void *mutex_worker(void* unused) {
    (void)unused;
    for(unsigned n=0;n<6000;++n) {
        ores_std_sync_result r=request_retry(&mutex,ORES_STD_WRITE);
        wait_until_started(&mutex,r.ticket);
        assert(atomic_fetch_add(&overlap,1u)==0u);
        ++mutex_counter;
        assert(atomic_fetch_sub(&overlap,1u)==1u);
        end_retry(&mutex,r.ticket);
        assert(release_retry(&mutex,r.ticket).code == ORES_STD_SYNC_GRANTED);
    }
    return 0;
}
static void *rw_reader(void* unused) {
    (void)unused;
    for(unsigned n=0;n<5000;++n) {
        ores_std_sync_result r=request_retry(&rw,ORES_STD_READ);
        wait_until_started(&rw,r.ticket);
        uint64_t snapshot=writer_counter;
        (void)snapshot;
        end_retry(&rw,r.ticket);
        assert(release_retry(&rw,r.ticket).code == ORES_STD_SYNC_GRANTED);
    }
    return 0;
}
static void *rw_writer(void* unused) {
    (void)unused;
    for(unsigned n=0;n<5000;++n) {
        ores_std_sync_result r=request_retry(&rw,ORES_STD_WRITE);
        wait_until_started(&rw,r.ticket);
        ++writer_counter;
        end_retry(&rw,r.ticket);
        assert(release_retry(&rw,r.ticket).code == ORES_STD_SYNC_GRANTED);
    }
    return 0;
}

int main(void) {
    test_scope_reincarnation();
    test_mutex_fifo_and_cancellation();
    test_rw_fairness_and_wake_cohorts();
    test_seal_and_physical_quiescence();
    test_stop_vs_start_race();
    test_closed_start_does_not_reactivate_dormant_grant();
    pthread_t mt[5],rt[4],wt[2];
    for(unsigned i=0;i<5;++i) assert(!pthread_create(&mt[i],NULL,mutex_worker,NULL));
    for(unsigned i=0;i<4;++i) assert(!pthread_create(&rt[i],NULL,rw_reader,NULL));
    for(unsigned i=0;i<2;++i) assert(!pthread_create(&wt[i],NULL,rw_writer,NULL));
    for(unsigned i=0;i<5;++i) assert(!pthread_join(mt[i],NULL));
    for(unsigned i=0;i<4;++i) assert(!pthread_join(rt[i],NULL));
    for(unsigned i=0;i<2;++i) assert(!pthread_join(wt[i],NULL));
    assert(mutex_counter==30000);
    assert(writer_counter==10000);
    printf("Oreslang native stdlib sync scheduler admission PASS\n");
    return 0;
}
