#include "oreslang/runtime/SourceActorBootstrap.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static void check(int ok) { if (!ok) abort(); }

static void actor_init(OresActorI64Heap* heap, uint64_t domain, uint64_t epoch) {
    (void)heap;
    check(domain != 0 && epoch != 0);
    int64_t* frame = NULL;
    check(oreslang_source_actor_frame_i64_slot_checked(
        domain, epoch, 0, &frame) && frame != NULL);
    check(*frame == 0); // initialized fresh at source actor construction
    *frame = 77;
    int64_t* denied = frame;
    check(!oreslang_source_actor_frame_i64_slot_checked(
        domain, epoch + 1, 0, &denied) && denied == NULL);
    check(!oreslang_source_actor_frame_i64_slot_checked(
        domain, epoch, 8192, &denied) && denied == NULL);
}

static int64_t actor_message(
        OresActorI64Heap* heap,
        uint64_t domain,
        uint64_t epoch,
        int64_t payload) {
    (void)heap;
    check(domain != 0 && epoch != 0);
    int64_t* word = NULL;
    check(oreslang_source_actor_frame_i64_slot_checked(
        domain, epoch, 0, &word) && word != NULL);
    /* This is a fresh per-turn scalar slot, not a carrier-local static. */
    check(*word == 0);
    *word = payload;
    check(*oreslang_source_actor_frame_i64_slot(domain, epoch, 0) == payload);
    int64_t* invalid = word;
    check(!oreslang_source_actor_frame_i64_slot_checked(
        domain + 1, epoch, 0, &invalid) && invalid == NULL);
    return *word;
}

static int64_t resumed(void* actor) {
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
    check(executions[0].context.id.runtime_domain
        == executions[1].context.id.runtime_domain);
    check(executions[0].context.allocation.kind
        == ORES_ALLOCATION_DOMAIN_ACTOR);
    check(executions[0].context.allocation.owner == 63);
    check(executions[1].context.allocation.owner == 63);
    check(executions[0].context.allocation.domain
        == executions[1].context.allocation.domain);
    check(executions[0].context.allocation.generation
        == executions[1].context.allocation.generation);

    check(continuations[0].record.boundary
        == ORES_CONTINUATION_BOUNDARY_AWAIT);
    check(continuations[0].record.source_token == UINT64_C(0x1234));
    /* A real actor Future must commit READY before the root continuation
     * can claim its physical CONTROL turn. Legacy PENDING trace claims are
     * deliberately insufficient for this guest await execution path. */
    check(continuations[0].record.state == ORES_CONTINUATION_CLAIMED);
    check(continuations[0].record.code_generation == 1);

    oreslang_source_actor_send(actor, 1);
    oreslang_source_actor_send(actor, 2);
    return 43;
}

int main(void) {
    int64_t* supervisor = NULL;
    check(!oreslang_source_actor_frame_i64_slot_checked(
        1, 1, 0, &supervisor) && supervisor == NULL);
    void* actor = oreslang_source_actor_spawn(
        actor_init, actor_message, 1);
    check(actor != NULL);

    const int64_t result =
        oreslang_source_actor_await_ready(actor, resumed, UINT64_C(0x1234));
    check(result == 43);

    OresNativeExecutionHandle outside = {0, 0};
    check(!oreslang_source_actor_current_execution_handle(&outside));

    oreslang_source_actor_stop(actor);
    int64_t* retired = NULL;
    check(!oreslang_source_actor_frame_i64_slot_checked(
        1, 1, 0, &retired) && retired == NULL);
    puts("SOURCE_ACTOR_AWAIT_TRACE_RUNTIME_PASS");
    return 0;
}
