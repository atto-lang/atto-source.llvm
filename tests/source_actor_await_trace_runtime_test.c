#include "oreslang/runtime/SourceActorBootstrap.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void check(int ok) { if (!ok) abort(); }

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
    return payload;
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

    /*
     * throw/raise/panic all snapshot the same live logical ancestry at their
     * recording point. Language-specific propagation remains a separate layer.
     */
    const OresNativeFailureKind kinds[3] = {
        ORES_NATIVE_FAILURE_THROW,
        ORES_NATIVE_FAILURE_RAISE,
        ORES_NATIVE_FAILURE_PANIC
    };
    const char* names[3] = {"THROW", "RAISE", "PANIC"};
    for (size_t i = 0; i < 3; ++i) {
        OresNativeFailureTraceHeader failure = {0};
        OresNativeExecutionTraceFrame failure_executions[4] = {0};
        OresNativeContinuationTraceFrame failure_continuations[4] = {0};
        const uint64_t source = UINT64_C(0x2001) + (uint64_t)i;
        check(oreslang_source_actor_capture_failure_trace(
            kinds[i],
            source,
            failure_executions,
            4,
            failure_continuations,
            4,
            &failure));
        check(failure.kind == kinds[i]);
        check(failure.source_token == source);
        check(failure.logical.execution_count == 2);
        check(failure.logical.continuation_count == 1);
        check(!failure.logical.corrupt);
        check(failure.current_execution.slot == current.slot);
        check(failure.current_execution.generation == current.generation);
        check(failure_continuations[0].record.boundary
            == ORES_CONTINUATION_BOUNDARY_AWAIT);

        char rendered[1024];
        const size_t rendered_bytes = ores_native_failure_trace_format(
            &failure,
            failure_executions,
            failure_continuations,
            rendered,
            sizeof(rendered));
        check(rendered_bytes != 0);
        check(strstr(rendered, names[i]) != NULL);
        check(strstr(rendered, "boundary[0] AWAIT") != NULL);
        if (kinds[i] == ORES_NATIVE_FAILURE_PANIC)
            fprintf(stderr, "SOURCE_FAILURE_TRACE_SAMPLE\n%s", rendered);
    }

    oreslang_source_actor_send(actor, 1);
    oreslang_source_actor_send(actor, 2);
    return 43;
}

int main(void) {
    void* actor = oreslang_source_actor_spawn(
        actor_init, actor_message, 1);
    check(actor != NULL);

    const int64_t result =
        oreslang_source_actor_await_ready(actor, resumed, UINT64_C(0x1234));
    check(result == 43);

    OresNativeExecutionHandle outside = {0, 0};
    check(!oreslang_source_actor_current_execution_handle(&outside));

    oreslang_source_actor_stop(actor);
    puts("SOURCE_ACTOR_AWAIT_TRACE_RUNTIME_PASS");
    return 0;
}
