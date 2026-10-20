#include "oreslang/runtime/NativeFailureTrace.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void check(int ok) { if (!ok) abort(); }

static void verify_kind(
        OresNativeExecutionContextStore* executions,
        OresNativeContinuationStore* continuations,
        OresNativeExecutionHandle current,
        OresNativeFailureTraceKind kind,
        const char* name) {
    OresNativeExecutionTraceFrame execution_frames[4] = {0};
    OresNativeContinuationTraceFrame continuation_frames[4] = {0};
    OresNativeFailureTraceReport report = {0};

    check(ores_native_failure_trace_capture(
        executions,
        continuations,
        current,
        kind,
        77,
        execution_frames,
        4,
        continuation_frames,
        4,
        &report));
    check(report.kind == kind);
    check(report.code == 77);
    check(report.current_execution.slot == current.slot);
    check(report.current_execution.generation == current.generation);
    check(report.logical.execution_count == 2);
    check(report.logical.continuation_count == 1);
    check(!report.logical.corrupt);

    check(ores_native_failure_trace_render(
        &report, execution_frames, continuation_frames, NULL, 0));
    check(report.required_bytes > 0);
    check(report.rendered_bytes == 0);
    check(!report.text_truncated);

    char tiny[16];
    check(ores_native_failure_trace_render(
        &report, execution_frames, continuation_frames,
        tiny, sizeof(tiny)));
    check(report.text_truncated);
    check(tiny[sizeof(tiny) - 1] == '\0');

    char text[2048];
    check(ores_native_failure_trace_render(
        &report, execution_frames, continuation_frames,
        text, sizeof(text)));
    check(!report.text_truncated);
    check(report.rendered_bytes == report.required_bytes);
    check(strstr(text, name) == text);
    check(strstr(text, "code=77") != NULL);
    check(strstr(text, "allocation=actor:90/7@3") != NULL);
    check(strstr(text, "boundary await source=55 codegen=9") != NULL);
    check(strstr(text, "origin=90:1") != NULL);
}

int main(void) {
    OresNativeExecutionContextStore* executions =
        ores_native_execution_context_store_open(8);
    check(executions != NULL);
    OresNativeContinuationStore* continuations =
        ores_native_continuation_store_open(8, executions);
    check(continuations != NULL);

    OresNativeExecutionContext origin = {
        .id = {90, 1},
        .allocation = {
            ORES_ALLOCATION_DOMAIN_ACTOR, 90, 7, 3
        },
        .parent_execution = {0, 0},
        .continuation = {0, 0}
    };
    OresNativeExecutionHandle first = {0, 0};
    check(ores_native_execution_context_store_insert(
        executions, &origin, &first));

    OresNativeSuspensionLink link = {0};
    check(ores_native_await_trace_suspend(
        executions,
        continuations,
        first,
        (OresNativeContinuationHandle){0, 0},
        ORES_CONTINUATION_BOUNDARY_AWAIT,
        9,
        55,
        &link));
    check(ores_native_execution_context_store_release(
        executions, first));

    OresNativeExecutionHandle resumed = {0, 0};
    OresNativeExecutionContext resumed_context = {0};
    check(ores_native_await_trace_resume_begin(
        executions,
        continuations,
        (OresNativeExecutionId){90, 2},
        link,
        &resumed,
        &resumed_context));
    check(ores_native_await_trace_resume_finish(
        continuations, link));

    verify_kind(
        executions, continuations, resumed,
        ORES_FAILURE_TRACE_THROW, "throw");
    verify_kind(
        executions, continuations, resumed,
        ORES_FAILURE_TRACE_RAISE, "raise");
    verify_kind(
        executions, continuations, resumed,
        ORES_FAILURE_TRACE_PANIC, "panic");

    OresNativeExecutionTraceFrame execution_frames[2] = {0};
    OresNativeContinuationTraceFrame continuation_frames[2] = {0};
    OresNativeFailureTraceReport invalid = {0};
    check(!ores_native_failure_trace_capture(
        executions,
        continuations,
        resumed,
        (OresNativeFailureTraceKind)0,
        1,
        execution_frames,
        2,
        continuation_frames,
        2,
        &invalid));

    const OresNativeExecutionHandle stale = resumed;
    check(ores_native_execution_context_store_release(
        executions, resumed));
    check(!ores_native_failure_trace_capture(
        executions,
        continuations,
        stale,
        ORES_FAILURE_TRACE_PANIC,
        1,
        execution_frames,
        2,
        continuation_frames,
        2,
        &invalid));

    check(ores_native_continuation_store_release(
        continuations, link.continuation));
    check(ores_native_continuation_store_live(continuations) == 0);
    check(ores_native_execution_context_store_live(executions) == 0);
    check(ores_native_continuation_store_close(continuations));
    check(ores_native_execution_context_store_close(executions));

    puts("NATIVE_FAILURE_TRACE_RENDER_PASS");
    return 0;
}
