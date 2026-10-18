#include "oreslang/runtime/OresCoreSync.h"
#include <pthread.h>
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <sched.h>

static ores_core_mutex_gate mutex = ORES_CORE_MUTEX_GATE_INIT;
static ores_core_rw_gate rw = ORES_CORE_RW_GATE_INIT;
static atomic_uint errors = ATOMIC_VAR_INIT(0u);
static atomic_uint inside = ATOMIC_VAR_INIT(0u);
static uint64_t guarded_counter;
static uint64_t rw_value;

static void *mutex_worker(void *arg) {
    (void)arg;
    for (unsigned i=0;i<50000;++i) {
        unsigned int t = 0;
        while (ores_core_mutex_try_acquire(&mutex, &t) == ORES_CORE_SYNC_BUSY) sched_yield();
        if (atomic_fetch_add(&inside,1u)!=0) atomic_fetch_add(&errors,1u);
        ++guarded_counter;
        if (atomic_fetch_sub(&inside,1u)!=1) atomic_fetch_add(&errors,1u);
        assert(ores_core_mutex_release(&mutex,t)==ORES_CORE_SYNC_OK);
        assert(ores_core_mutex_release(&mutex,t)==ORES_CORE_SYNC_STALE);
    }
    return NULL;
}
static void *reader(void* unused) {
    (void)unused;
    for(unsigned i=0;i<20000;++i) {
        while(ores_core_rw_try_read(&rw)==ORES_CORE_SYNC_BUSY) sched_yield();
        uint64_t copy=rw_value;
        (void)copy;
        assert(ores_core_rw_read_release(&rw)==ORES_CORE_SYNC_OK);
    }
    return NULL;
}
static void *writer(void *unused) {
    (void)unused;
    ores_core_rw_writer_wait wait = ORES_CORE_RW_WRITER_WAIT_INIT;
    for(unsigned i=0;i<20000;++i) {
        assert(ores_core_rw_writer_register(&rw,&wait)==ORES_CORE_SYNC_OK);
        unsigned int t=0;
        while(ores_core_rw_try_write(&rw,&wait,&t)==ORES_CORE_SYNC_BUSY) sched_yield();
        assert(ores_core_rw_writer_unregister(&wait)==ORES_CORE_SYNC_OK);
        ++rw_value;
        assert(ores_core_rw_write_release(&rw,t)==ORES_CORE_SYNC_OK);
        assert(ores_core_rw_write_release(&rw,t)==ORES_CORE_SYNC_STALE);
    }
    return NULL;
}
int main(void) {
    /* Invalid/unpaired operations and terminal epoch exhaustion fail closed. */
    assert(ores_core_mutex_release(&mutex,0u)==ORES_CORE_SYNC_INVALID);
    assert(ores_core_rw_read_release(&rw)==ORES_CORE_SYNC_STALE);
    {
        ores_core_mutex_gate e = ORES_CORE_MUTEX_GATE_INIT;
        e.issued = UINT_MAX-1u;
        unsigned int token = 0;
        assert(ores_core_mutex_try_acquire(&e,&token)==ORES_CORE_SYNC_EXHAUSTED);
        assert(atomic_load(&e.owner)==0u);
        assert(token==0u);
        ores_core_rw_gate x = ORES_CORE_RW_GATE_INIT;
        x.issued_writers = ORES_CORE_RW_MAX_READERS;
        assert(ores_core_rw_try_write(&x,0,&token)==ORES_CORE_SYNC_EXHAUSTED);
        assert(atomic_load(&x.state)==0u);
    }
    pthread_t ts[8];
    for(int i=0;i<8;++i) assert(pthread_create(&ts[i],0,mutex_worker,0)==0);
    for(int i=0;i<8;++i) assert(pthread_join(ts[i],0)==0);
    assert(guarded_counter==400000);
    assert(atomic_load(&errors)==0);
    ores_core_rw_writer_wait wait=ORES_CORE_RW_WRITER_WAIT_INIT;
    assert(ores_core_rw_writer_register(&rw,&wait)==ORES_CORE_SYNC_OK);
    assert(ores_core_rw_try_read(&rw)==ORES_CORE_SYNC_BUSY);
    assert(ores_core_rw_writer_unregister(&wait)==ORES_CORE_SYNC_OK);
    assert(ores_core_rw_writer_unregister(&wait)==ORES_CORE_SYNC_STALE);
    pthread_t readers[5],writers[3];
    for(int i=0;i<5;++i) assert(pthread_create(&readers[i],0,reader,0)==0);
    for(int i=0;i<3;++i) assert(pthread_create(&writers[i],0,writer,0)==0);
    for(int i=0;i<5;++i) assert(pthread_join(readers[i],0)==0);
    for(int i=0;i<3;++i) assert(pthread_join(writers[i],0)==0);
    assert(rw_value==60000);
    puts("native core mutex/rw concurrency PASS");
    return 0;
}
