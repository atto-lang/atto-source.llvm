#include "oreslang/runtime/NativeFailureTrace.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void check_impl(int ok, const char* expr, int line) {
    if (!ok) {
        fprintf(stderr, "FAILURE_TRACE_CHECK_FAIL line=%d expr=%s\n", line, expr);
        abort();
    }
}
#define check(expr) check_impl((expr), #expr, __LINE__)

static void verify_kind(
        OresNativeExecutionContextStore* executions,
        OresNativeContinuationStore* continuations,
        OresNativeExecutionHandle current,
        OresNativeFailureKind kind,
        const char* name,
        uint64_t source_token) {
    OresNativeExecutionTraceFrame execution_frames[4] = {0};
    OresNativeContinuationTraceFrame continuation_frames[4] = {0};
    OresNativeFailureTraceHeader header = {0};

    check(ores_native_failure_trace_capture(
        executions,
        continuations,
        current,
        kind,
        source_token,
        execution_frames,
        4,
        continuation_frames,
        4,
        &header));
    check(header.kind == kind);
    check(header.source_token == source_token);
    check(header.current_execution.slot == current.slot);
    check(header.current_execution.generation == current.generation);
    check(header.current_execution_id.runtime_domain == 90);
    check(header.current_execution_id.execution == 2);
    check(header.allocation.kind == ORES_ALLOCATION_DOMAIN_ACTOR);
    check(header.allocation.domain == 90);
    check(header.allocation.owner == 7);
    check(header.allocation.generation == 3);
    check(header.logical.execution_count == 2);
    check(header.logical.continuation_count == 1);
    check(!header.logical.corrupt);

    char text[2048];
    const size_t written = ores_native_failure_trace_format_checked(
        &header,
        execution_frames, 4,
        continuation_frames, 4,
        text,
        sizeof(text));
    check(written != 0);
    check(strstr(text, name) == text);
    check(strstr(text, "execution[0] id=90:2") != NULL);
    check(strstr(text, "execution[1] id=90:1") != NULL);
    check(strstr(text, "boundary[0] AWAIT source=55 code_generation=9")
        != NULL);

    char tiny[16];
    const size_t tiny_written = ores_native_failure_trace_format(
        &header,
        execution_frames,
        continuation_frames,
        tiny,
        sizeof(tiny));
    check(tiny_written == sizeof(tiny) - 1);
    check(tiny[sizeof(tiny) - 1] == '\0');

    char checked[2048];
    check(ores_native_failure_trace_format_checked(
        &header, execution_frames, 4, continuation_frames, 4,
        checked, sizeof(checked)) == written);
    check(strcmp(text, checked) == 0);
    char single[1] = {'x'};
    check(ores_native_failure_trace_format_checked(
        &header, execution_frames, 4, continuation_frames, 4,
        single, sizeof(single)) == 0);
    check(single[0] == '\0');

    /* Invalid input must not publish a stale previous trace. Bounds are
     * rejected before indexing either caller-owned frame array. */
    strcpy(checked, "stale trace");
    check(ores_native_failure_trace_format_checked(
        &header, execution_frames, 1, continuation_frames, 4,
        checked, sizeof(checked)) == 0);
    check(checked[0] == '\0');
    strcpy(checked, "stale trace");
    check(ores_native_failure_trace_format_checked(
        &header, execution_frames, 4, continuation_frames, 0,
        checked, sizeof(checked)) == 0);
    check(checked[0] == '\0');
    strcpy(checked, "stale trace");
    check(ores_native_failure_trace_format_checked(
        NULL, execution_frames, 4, continuation_frames, 4,
        checked, sizeof(checked)) == 0);
    check(checked[0] == '\0');
    strcpy(checked, "stale trace");
    check(ores_native_failure_trace_format(
        NULL, execution_frames, continuation_frames,
        checked, sizeof(checked)) == 0);
    check(checked[0] == '\0');

    OresNativeFailureTraceHeader malformed = header;
    malformed.logical.execution_count = SIZE_MAX;
    check(ores_native_failure_trace_format_checked(
        &malformed, execution_frames, 4, continuation_frames, 4,
        checked, sizeof(checked)) == 0);
    check(checked[0] == '\0');
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
        ORES_NATIVE_FAILURE_THROW, "THROW", 1001);
    verify_kind(
        executions, continuations, resumed,
        ORES_NATIVE_FAILURE_RAISE, "RAISE", 1002);
    verify_kind(
        executions, continuations, resumed,
        ORES_NATIVE_FAILURE_PANIC, "PANIC", 1003);

    OresNativeExecutionTraceFrame execution_frames[2] = {0};
    OresNativeContinuationTraceFrame continuation_frames[2] = {0};
    OresNativeFailureTraceHeader invalid;
    OresNativeFailureTraceHeader empty = {0};
    memset(&invalid, 0xa5, sizeof(invalid));
    check(!ores_native_failure_trace_capture(
        executions,
        continuations,
        resumed,
        (OresNativeFailureKind)0,
        1,
        execution_frames,
        2,
        continuation_frames,
        2,
        &invalid));
    check(memcmp(&invalid, &empty, sizeof(invalid)) == 0);

    const OresNativeExecutionHandle stale = resumed;
    check(ores_native_execution_context_store_release(
        executions, resumed));
    memset(&invalid, 0xa5, sizeof(invalid));
    check(!ores_native_failure_trace_capture(
        executions,
        continuations,
        stale,
        ORES_NATIVE_FAILURE_PANIC,
        1,
        execution_frames,
        2,
        continuation_frames,
        2,
        &invalid));
    check(memcmp(&invalid, &empty, sizeof(invalid)) == 0);

    check(ores_native_continuation_store_release(
        continuations, link.continuation));
    check(ores_native_continuation_store_live(continuations) == 0);
    check(ores_native_execution_context_store_live(executions) == 0);
    check(ores_native_continuation_store_close(continuations));
    check(ores_native_execution_context_store_close(executions));

    puts("NATIVE_FAILURE_TRACE_CAPTURE_PASS");
    return 0;
}
