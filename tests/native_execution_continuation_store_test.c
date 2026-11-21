#define _POSIX_C_SOURCE 200809L
#include "oreslang/runtime/NativeAwaitTraceBridge.h"
#include "oreslang/runtime/NativeLogicalTrace.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static void check_impl(int ok, const char* expression, int line) {
    if (!ok) {
        fprintf(stderr, "CHECK_FAIL line=%d expr=%s\n", line, expression);
        abort();
    }
}
#define check(expression) check_impl((expression), #expression, __LINE__)

typedef struct {
    OresNativeExecutionContextStore* executions;
    OresNativeContinuationStore* continuations;
    OresNativeExecutionId resumed_id;
    OresNativeSuspensionLink link;
    OresNativeExecutionHandle execution;
    OresNativeExecutionContext context;
    int ok;
} ResumeArgs;

static void* resume_on_fresh_worker_stack(void* raw) {
    ResumeArgs* args = (ResumeArgs*)raw;
    volatile uint64_t worker_stack_noise[32];
    for (size_t i = 0; i < 32; ++i)
        worker_stack_noise[i] = UINT64_C(0xa5a5000000000000) + i;
    args->ok = ores_native_await_trace_resume_begin(
        args->executions,
        args->continuations,
        args->resumed_id,
        args->link,
        &args->execution,
        &args->context);
    if (args->ok)
        args->ok = ores_native_await_trace_resume_finish(
            args->continuations, args->link);
    check(worker_stack_noise[31] == UINT64_C(0xa5a500000000001f));
    return NULL;
}

static OresNativeExecutionHandle resume_on_new_thread(
        OresNativeExecutionContextStore* executions,
        OresNativeContinuationStore* continuations,
        OresNativeSuspensionLink link,
        uint64_t execution_id) {
    ResumeArgs args = {
        .executions = executions,
        .continuations = continuations,
        .resumed_id = {link.execution_id.runtime_domain, execution_id},
        .link = link
    };
    pthread_t thread;
    check(pthread_create(&thread, NULL, resume_on_fresh_worker_stack, &args) == 0);
    check(pthread_join(thread, NULL) == 0);
    check(args.ok);
    check(args.context.id.execution == execution_id);
    check(args.context.id.runtime_domain == link.execution_id.runtime_domain);
    check(args.context.parent_execution.slot == link.parent.slot);
    check(args.context.parent_execution.generation == link.parent.generation);
    check(!ores_native_store_handle_valid(args.context.continuation));
    return args.execution;
}

static OresNativeSuspensionLink suspend_and_drop_stack(
        OresNativeExecutionContextStore* executions,
        OresNativeContinuationStore* continuations,
        OresNativeExecutionHandle execution,
        OresNativeContinuationHandle parent,
        OresNativeContinuationBoundary boundary,
        uint64_t code_generation,
        uint64_t source_token) {
    volatile uint64_t dead_stack[64];
    for (size_t i = 0; i < 64; ++i)
        dead_stack[i] = UINT64_C(0x5a5a000000000000) + i;

    OresNativeSuspensionLink link = {0};
    check(ores_native_await_trace_suspend(
        executions,
        continuations,
        execution,
        parent,
        boundary,
        code_generation,
        source_token,
        &link));
    check(link.execution_id.runtime_domain != 0);
    check(link.execution_id.execution != 0);
    check(ores_native_store_handle_valid(link.continuation));
    check(ores_native_store_handle_valid(link.parent));

    /* Keep the volatile writes observable until after the persisted link exists. */
    check(dead_stack[63] == UINT64_C(0x5a5a00000000003f));
    return link;
}

static void check_allocation(
        OresNativeExecutionContext context,
        OresNativeAllocationDomainRef expected) {
    check(context.allocation.kind == expected.kind);
    check(context.allocation.domain == expected.domain);
    check(context.allocation.owner == expected.owner);
    check(context.allocation.generation == expected.generation);
}

int main(void) {
    check(ORES_NATIVE_AOSOA_LANES == 8u);

    OresNativeExecutionContextStore* executions =
        ores_native_execution_context_store_open(24);
    check(executions != NULL);
    OresNativeContinuationStore* continuations =
        ores_native_continuation_store_open(24, executions);
    check(continuations != NULL);

    const OresNativeAllocationDomainRef actor_heap = {
        ORES_ALLOCATION_DOMAIN_ACTOR, 77, 0, 9
    };
    const OresNativeExecutionContext root = {
        .id = {77, 100},
        .allocation = actor_heap,
        .parent_execution = {0, 0},
        .continuation = {0, 0}
    };
    OresNativeExecutionHandle e1 = {0, 0};
    check(ores_native_execution_context_store_insert(executions, &root, &e1));

    /*
     * Every store instance owns its own non-recycled public slot range.
     * Generation-only checks would incorrectly accept an equal {slot=1,gen=1}
     * from another store or a newly opened replacement instance.
     */
    OresNativeExecutionContextStore* foreign_executions =
        ores_native_execution_context_store_open(2);
    check(foreign_executions != NULL);
    OresNativeExecutionHandle foreign_execution = {0, 0};
    check(ores_native_execution_context_store_insert(
        foreign_executions, &root, &foreign_execution));
    check(foreign_execution.slot != e1.slot);
    OresNativeExecutionContext probe = {0};
    check(!ores_native_execution_context_store_get(
        executions, foreign_execution, &probe));
    check(!ores_native_execution_context_store_get(
        foreign_executions, e1, &probe));
    check(!ores_native_execution_context_store_retain(
        executions, foreign_execution));
    check(!ores_native_execution_context_store_release(
        executions, foreign_execution));

    OresNativeContinuationStore* foreign_continuations =
        ores_native_continuation_store_open(2, executions);
    check(foreign_continuations != NULL);
    OresNativeContinuationHandle c_local = {0, 0}, c_foreign = {0, 0};
    check(ores_native_continuation_store_publish(
        continuations, e1, (OresNativeContinuationHandle){0, 0},
        ORES_CONTINUATION_BOUNDARY_AWAIT, 1, 10, &c_local));
    check(ores_native_continuation_store_publish(
        foreign_continuations, e1, (OresNativeContinuationHandle){0, 0},
        ORES_CONTINUATION_BOUNDARY_AWAIT, 1, 11, &c_foreign));
    check(c_local.slot != c_foreign.slot);
    OresNativeContinuationRecord c_probe = {0};
    check(!ores_native_continuation_store_get(
        continuations, c_foreign, &c_probe));
    check(!ores_native_continuation_store_get(
        foreign_continuations, c_local, &c_probe));
    check(!ores_native_continuation_store_try_claim_resume(
        continuations, c_foreign, &c_probe));
    check(!ores_native_continuation_store_cancel(
        foreign_continuations, c_local));
    check(!ores_native_continuation_store_release(
        continuations, c_foreign));
    check(ores_native_continuation_store_release(
        continuations, c_local));
    check(ores_native_continuation_store_release(
        foreign_continuations, c_foreign));
    check(ores_native_continuation_store_close(foreign_continuations));

    check(ores_native_execution_context_store_release(
        foreign_executions, foreign_execution));
    check(ores_native_execution_context_store_close(foreign_executions));
    foreign_executions = ores_native_execution_context_store_open(2);
    check(foreign_executions != NULL);
    OresNativeExecutionHandle replacement_execution = {0, 0};
    check(ores_native_execution_context_store_insert(
        foreign_executions, &root, &replacement_execution));
    check(replacement_execution.slot != foreign_execution.slot);
    check(!ores_native_execution_context_store_get(
        foreign_executions, foreign_execution, &probe));
    check(ores_native_execution_context_store_release(
        foreign_executions, replacement_execution));
    check(ores_native_execution_context_store_close(foreign_executions));


    /*
     * Actor boundary -> await -> await -> supervisor CONTROL. Each resume
     * happens on a new pthread after the suspending C stack has returned.
     */
    OresNativeSuspensionLink actor_link = suspend_and_drop_stack(
        executions, continuations, e1, (OresNativeContinuationHandle){0, 0},
        ORES_CONTINUATION_BOUNDARY_ACTOR_MESSAGE, 7, 1001);
    check(ores_native_execution_context_store_release(executions, e1));
    OresNativeExecutionHandle e2 =
        resume_on_new_thread(executions, continuations, actor_link, 101);

    OresNativeSuspensionLink await_link = suspend_and_drop_stack(
        executions, continuations, e2, actor_link.continuation,
        ORES_CONTINUATION_BOUNDARY_AWAIT, 7, 1002);
    check(ores_native_execution_context_store_release(executions, e2));
    OresNativeExecutionHandle e3 =
        resume_on_new_thread(executions, continuations, await_link, 102);

    OresNativeSuspensionLink second_await_link = suspend_and_drop_stack(
        executions, continuations, e3, await_link.continuation,
        ORES_CONTINUATION_BOUNDARY_AWAIT, 7, 1003);
    check(ores_native_execution_context_store_release(executions, e3));
    OresNativeExecutionHandle e4 =
        resume_on_new_thread(executions, continuations, second_await_link, 103);

    OresNativeSuspensionLink supervisor_link = suspend_and_drop_stack(
        executions, continuations, e4, second_await_link.continuation,
        ORES_CONTINUATION_BOUNDARY_SUPERVISOR_CONTROL, 8, 1004);
    check(ores_native_execution_context_store_release(executions, e4));
    OresNativeExecutionHandle e5 =
        resume_on_new_thread(executions, continuations, supervisor_link, 104);

    OresNativeExecutionTraceFrame execution_frames[8];
    OresNativeContinuationTraceFrame continuation_frames[8];
    OresNativeLogicalTraceResult trace;
    check(ores_native_logical_trace_snapshot(
        executions, continuations, e5,
        execution_frames, 8,
        continuation_frames, 8,
        &trace));
    check(!trace.corrupt);
    check(!trace.execution_truncated);
    check(!trace.continuation_truncated);
    check(trace.execution_count == 5);
    check(trace.continuation_count == 4);

    check(execution_frames[0].context.id.execution == 104);
    check(execution_frames[1].context.id.execution == 103);
    check(execution_frames[2].context.id.execution == 102);
    check(execution_frames[3].context.id.execution == 101);
    check(execution_frames[4].context.id.execution == 100);
    for (size_t i = 0; i < trace.execution_count; ++i)
        check_allocation(execution_frames[i].context, actor_heap);

    check(continuation_frames[0].record.boundary
        == ORES_CONTINUATION_BOUNDARY_SUPERVISOR_CONTROL);
    check(continuation_frames[1].record.boundary
        == ORES_CONTINUATION_BOUNDARY_AWAIT);
    check(continuation_frames[2].record.boundary
        == ORES_CONTINUATION_BOUNDARY_AWAIT);
    check(continuation_frames[3].record.boundary
        == ORES_CONTINUATION_BOUNDARY_ACTOR_MESSAGE);
    check(continuation_frames[0].record.code_generation == 8);
    check(continuation_frames[1].record.source_token == 1003);
    check(continuation_frames[2].record.source_token == 1002);
    check_allocation((OresNativeExecutionContext){
        .id = continuation_frames[0].record.execution_id,
        .allocation = continuation_frames[0].record.allocation
    }, actor_heap);

    OresNativeLogicalTraceResult bounded;
    check(ores_native_logical_trace_snapshot(
        executions, continuations, e5,
        execution_frames, 2,
        continuation_frames, 2,
        &bounded));
    check(bounded.execution_count == 2 && bounded.execution_truncated);
    check(bounded.continuation_count == 2 && bounded.continuation_truncated);

    /* Rendering zero execution frames must still discover causal continuations. */
    OresNativeLogicalTraceResult causal_only;
    check(ores_native_logical_trace_snapshot(
        executions, continuations, e5,
        NULL, 0,
        continuation_frames, 8,
        &causal_only));
    check(causal_only.execution_count == 0 && causal_only.execution_truncated);
    check(causal_only.continuation_count == 4);
    check(!causal_only.continuation_truncated && !causal_only.corrupt);

    /* The currently live execution owns no native stack pointer in either store. */
    check(sizeof(OresNativeExecutionContext) == 64);
    check(sizeof(OresNativeSuspensionLink) == 32);

    /* Tear down from newest to oldest; store references keep ancestors alive. */
    check(ores_native_execution_context_store_release(executions, e5));
    check(ores_native_continuation_store_release(
        continuations, supervisor_link.continuation));
    check(ores_native_continuation_store_release(
        continuations, second_await_link.continuation));
    check(ores_native_continuation_store_release(
        continuations, await_link.continuation));
    check(ores_native_continuation_store_release(
        continuations, actor_link.continuation));
    check(ores_native_execution_context_store_live(executions) == 0);
    check(ores_native_continuation_store_live(continuations) == 0);

    OresNativeExecutionContext stale_context;
    OresNativeContinuationRecord stale_continuation;
    check(!ores_native_execution_context_store_get(
        executions, e1, &stale_context));
    check(!ores_native_continuation_store_get(
        continuations, actor_link.continuation, &stale_continuation));

    /* A causal child cannot be derived from a continuation that never resumed. */
    OresNativeExecutionContext pending_origin_context = {
        .id = {77, 150},
        .allocation = actor_heap,
        .parent_execution = {0, 0},
        .continuation = {0, 0}
    };
    OresNativeExecutionContext pending_child_context = pending_origin_context;
    pending_child_context.id.execution = 151;
    OresNativeExecutionHandle pending_origin = {0, 0};
    OresNativeExecutionHandle pending_child = {0, 0};
    check(ores_native_execution_context_store_insert(
        executions, &pending_origin_context, &pending_origin));
    check(ores_native_execution_context_store_insert(
        executions, &pending_child_context, &pending_child));
    OresNativeContinuationHandle pending_parent = {0, 0};
    check(ores_native_continuation_store_publish(
        continuations, pending_origin, (OresNativeContinuationHandle){0, 0},
        ORES_CONTINUATION_BOUNDARY_AWAIT, 9, 1501, &pending_parent));
    OresNativeContinuationHandle invalid_child = {0, 0};
    check(!ores_native_continuation_store_publish(
        continuations, pending_child, pending_parent,
        ORES_CONTINUATION_BOUNDARY_AWAIT, 9, 1502, &invalid_child));
    check(ores_native_continuation_store_cancel(continuations, pending_parent));
    check(!ores_native_continuation_store_publish(
        continuations, pending_child, pending_parent,
        ORES_CONTINUATION_BOUNDARY_AWAIT, 9, 1503, &invalid_child));
    check(ores_native_continuation_store_release(continuations, pending_parent));
    check(ores_native_execution_context_store_release(executions, pending_origin));
    check(ores_native_execution_context_store_release(executions, pending_child));

/* Releasing a child may reach a still-CLAIMED parent. The child drop
 * succeeds exactly once; the physical frame remains pinned until finish. */
pending_origin_context.id.execution = 160;
pending_child_context.id.execution = 161;
check(ores_native_execution_context_store_insert(
    executions, &pending_origin_context, &pending_origin));
check(ores_native_execution_context_store_insert(
    executions, &pending_child_context, &pending_child));
check(ores_native_continuation_store_publish(
    continuations, pending_origin, (OresNativeContinuationHandle){0, 0},
    ORES_CONTINUATION_BOUNDARY_AWAIT, 9, 1601, &pending_parent));
check(ores_native_continuation_store_try_claim_resume(
    continuations, pending_parent, NULL));
check(ores_native_continuation_store_publish(
    continuations, pending_child, pending_parent,
    ORES_CONTINUATION_BOUNDARY_AWAIT, 9, 1602, &invalid_child));
check(ores_native_continuation_store_release(continuations, pending_parent));
check(ores_native_continuation_store_cancel(continuations, invalid_child));
check(ores_native_continuation_store_release(continuations, invalid_child));
check(!ores_native_continuation_store_release(continuations, invalid_child));
check(ores_native_continuation_store_get(
    continuations, pending_parent, &stale_continuation));
check(stale_continuation.state == ORES_CONTINUATION_CLAIMED);
check(!ores_native_continuation_store_release(continuations, pending_parent));
check(ores_native_continuation_store_finish_resume(continuations, pending_parent));
check(ores_native_continuation_store_release(continuations, pending_parent));
check(ores_native_execution_context_store_release(executions, pending_origin));
check(ores_native_execution_context_store_release(executions, pending_child));
check(ores_native_continuation_store_live(continuations) == 0);
check(ores_native_execution_context_store_live(executions) == 0);

    /* Cross an AoSoA block boundary and prove all nine lanes remain addressable. */
    OresNativeExecutionHandle lanes[9];
    for (uint64_t i = 0; i < 9; ++i) {
        OresNativeExecutionContext context = {
            .id = {77, 200 + i},
            .allocation = actor_heap,
            .parent_execution = {0, 0},
            .continuation = {0, 0}
        };
        check(ores_native_execution_context_store_insert(
            executions, &context, &lanes[i]));
    }
    check(ores_native_execution_context_store_live(executions) == 9);
    OresNativeExecutionContext duplicate = {
        .id = {77, 204},
        .allocation = actor_heap,
        .parent_execution = {0, 0},
        .continuation = {0, 0}
    };
    OresNativeExecutionHandle duplicate_handle = {0, 0};
    check(!ores_native_execution_context_store_insert(
        executions, &duplicate, &duplicate_handle));
    for (uint64_t i = 0; i < 9; ++i) {
        OresNativeExecutionContext context;
        check(ores_native_execution_context_store_get(
            executions, lanes[i], &context));
        check(context.id.execution == 200 + i);
        check(ores_native_execution_context_store_release(
            executions, lanes[i]));
    }

    check(ores_native_continuation_store_close(continuations));
    check(ores_native_execution_context_store_close(executions));
    puts("NATIVE_EXECUTION_CONTINUATION_AOSOA_TRACE_PASS");
    return 0;
}
