#include "oreslang/runtime/SourceActorBootstrap.h"

#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static atomic_int delivered;
static atomic_int first_entered;
static atomic_int release_first;
static atomic_int continuation_entered;
static atomic_int premature_resume;

static void check(int ok) {
    if (!ok) abort();
}

static void actor_init(OresActorI64Heap* heap, uint64_t domain, uint64_t epoch) {
    (void)heap;
    check(domain != 0 && epoch != 0);
}

static int64_t actor_message(
        OresActorI64Heap* heap,
        uint64_t domain,
        uint64_t epoch,
        int64_t payload) {
    (void)heap;
    check(domain != 0 && epoch != 0);
    if (payload == 7) {
        atomic_store_explicit(&first_entered, 1, memory_order_release);
        while (!atomic_load_explicit(&release_first, memory_order_acquire))
            sched_yield();
    }
    atomic_fetch_add_explicit(&delivered, 1, memory_order_acq_rel);
    return payload;
}

static int64_t resumed_after_done(void* actor) {
    (void)actor;
    atomic_store_explicit(&continuation_entered, 1, memory_order_release);

    /* Message 7 was already active. Message 8 was queued and must be
     * abandoned by source stop, not executed before get_done resumes. */
    check(atomic_load_explicit(&delivered, memory_order_acquire) == 1);

    OresNativeExecutionHandle current = {0, 0};
    check(oreslang_source_actor_current_execution_handle(&current));
    check(ores_native_store_handle_valid(current));

    OresNativeExecutionTraceFrame executions[4] = {0};
    OresNativeContinuationTraceFrame continuations[4] = {0};
    OresNativeLogicalTraceResult trace = {0};
    check(oreslang_source_actor_snapshot_current_trace(
        executions, 4, continuations, 4, &trace));
    check(!trace.corrupt && !trace.execution_truncated
        && !trace.continuation_truncated);
    check(trace.execution_count == 2);
    check(trace.continuation_count == 1);
    check(executions[0].context.id.execution
        != executions[1].context.id.execution);
    check(executions[0].context.allocation.kind
        == ORES_ALLOCATION_DOMAIN_ACTOR);
    check(executions[0].context.allocation.owner == 63);
    check(executions[1].context.allocation.owner == 63);
    check(continuations[0].record.boundary
        == ORES_CONTINUATION_BOUNDARY_AWAIT);
    check(continuations[0].record.source_token == UINT64_C(0x5678));
    return 43;
}

static void* release_after_probe(void* opaque) {
    (void)opaque;
    while (!atomic_load_explicit(&first_entered, memory_order_acquire))
        sched_yield();

    for (unsigned i = 0; i < 10000; ++i) {
        if (atomic_load_explicit(
                &continuation_entered, memory_order_acquire)) {
            atomic_store_explicit(
                &premature_resume, 1, memory_order_release);
            break;
        }
        sched_yield();
    }

    atomic_store_explicit(&release_first, 1, memory_order_release);
    return NULL;
}

int main(void) {
    void* actor = oreslang_source_actor_spawn(
        actor_init, actor_message, 1);
    check(actor != NULL);

    oreslang_source_actor_send(actor, 7);
    oreslang_source_actor_send(actor, 8);

    while (!atomic_load_explicit(&first_entered, memory_order_acquire))
        sched_yield();

    oreslang_source_actor_request_stop(actor);

    pthread_t releaser;
    check(pthread_create(&releaser, NULL, release_after_probe, NULL) == 0);

    const int64_t result =
        oreslang_source_actor_await_done(
            actor, resumed_after_done, UINT64_C(0x5678));
    check(result == 43);
    check(pthread_join(releaser, NULL) == 0);

    check(!atomic_load_explicit(&premature_resume, memory_order_acquire));
    check(atomic_load_explicit(&continuation_entered, memory_order_acquire));
    check(atomic_load_explicit(&delivered, memory_order_acquire) == 1);

    OresNativeExecutionHandle outside = {0, 0};
    check(!oreslang_source_actor_current_execution_handle(&outside));

    puts("SOURCE_ACTOR_AWAIT_DONE_TRACE_RUNTIME_PASS");
    return 0;
}
