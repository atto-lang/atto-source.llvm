#include "oreslang/runtime/NativeTraceStore.h"
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdint.h>

static void check(int good) { assert(good); }
static OresNativeExecutionContext value(uint64_t execution) {
    OresNativeExecutionContext v = {0};
    v.id.runtime_domain = 3;
    v.id.execution = execution;
    v.allocation.kind = ORES_ALLOCATION_DOMAIN_ACTOR;
    v.allocation.domain = 3;
    v.allocation.owner = 0;
    v.allocation.generation = 1;
    return v;
}
typedef struct { OresNativeTraceStore* store; atomic_int failures; } Worker;
static void* churn(void* raw) {
    Worker* w = raw;
    for (int i = 0; i < 2000; ++i) {
        OresNativeExecutionContext v = value((uint64_t)i + 1), fetched;
        OresNativeTraceHandle h = ores_native_execution_store_insert(w->store, &v);
        if (!h.store_id || !ores_native_execution_store_load(w->store, h, &fetched)
                || fetched.id.execution != v.id.execution
                || !ores_native_trace_store_release(w->store, h)
                || ores_native_execution_store_load(w->store, h, &fetched))
            atomic_fetch_add(&w->failures, 1);
    }
    return NULL;
}
int main(void) {
    OresNativeTraceStore* e = ores_native_execution_store_open(8);
    OresNativeTraceStore* c = ores_native_continuation_store_open(2);
    OresNativeTraceStore* other = ores_native_execution_store_open(1);
    check(e && c && other);
    OresNativeExecutionContext v = value(7), fetched = {0};
    OresNativeTraceHandle h = ores_native_execution_store_insert(e, &v);
    check(h.store_id && ores_native_execution_store_load(e,h,&fetched));
    check(fetched.id.execution == 7 && fetched.allocation.owner == 0);
    check(!ores_native_execution_store_load(other,h,&fetched));
    check(!ores_native_continuation_store_load(c,h,NULL));
    check(!ores_native_execution_store_insert(c,&v).store_id);
    check(ores_native_trace_store_release(e,h));
    check(!ores_native_trace_store_release(e,h));
    check(!ores_native_execution_store_load(e,h,&fetched));
    OresNativeTraceHandle second = ores_native_execution_store_insert(e,&v);
    check(second.store_id == h.store_id && second.slot_generation != h.slot_generation);
    check(!ores_native_execution_store_load(e,h,&fetched));
    OresNativeContinuationRecord cont = {0};
    cont.parent_execution = second;
    cont.source_span = 0xF1;
    OresNativeTraceHandle ch = ores_native_continuation_store_insert(c,&cont);
    OresNativeContinuationRecord out = {0};
    check(ch.store_id && ores_native_continuation_store_load(c,ch,&out));
    check(out.parent_execution.store_id == second.store_id && out.source_span == 0xF1);
    check(!ores_native_trace_store_release(e,ch));
    check(ores_native_trace_store_release(c,ch));
    check(!ores_native_continuation_store_load(c,ch,&out));
    check(ores_native_trace_store_release(e,second));
    Worker w = {.store=e};
    pthread_t workers[8];
    for (size_t i=0;i<8;i++) check(pthread_create(&workers[i],NULL,churn,&w)==0);
    for (size_t i=0;i<8;i++) check(pthread_join(workers[i],NULL)==0);
    check(atomic_load(&w.failures)==0);
    check(!ores_native_execution_store_open(0));
    ores_native_trace_store_close(other);
    ores_native_trace_store_close(c);
    ores_native_trace_store_close(e);
    puts("NATIVE_TRACE_STORE_PASS");
    return 0;
}
