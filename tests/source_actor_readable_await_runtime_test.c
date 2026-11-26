#include "oreslang/runtime/SourceActorBootstrap.h"

#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

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
    check(continuations[0].record.boundary
        == ORES_CONTINUATION_BOUNDARY_AWAIT);
    check(continuations[0].record.source_token == UINT64_C(0x5151));
    check(continuations[0].record.state == ORES_CONTINUATION_CLAIMED);
    check(continuations[0].record.code_generation == 1);

    /* Prove guest-visible work is admitted only after actor-lane resume. */
    oreslang_source_actor_send(actor, 7);
    return 43;
}

typedef struct {
    int fd;
} Writer;

static void* writer_main(void* opaque) {
    Writer* writer = (Writer*)opaque;
    const struct timespec delay = { .tv_sec = 0, .tv_nsec = 20 * 1000 * 1000 };
    check(nanosleep(&delay, NULL) == 0);
    const unsigned char byte = 0x41;
    check(write(writer->fd, &byte, 1) == 1);
    return NULL;
}

int main(void) {
    int pipefd[2] = {-1, -1};
    check(pipe(pipefd) == 0);

    const int flags = fcntl(pipefd[0], F_GETFL);
    check(flags >= 0);
    check(fcntl(pipefd[0], F_SETFL, flags | O_NONBLOCK) == 0);

    void* actor = oreslang_source_actor_spawn(
        actor_init, actor_message, 1);
    check(actor != NULL);

    Writer writer = { .fd = pipefd[1] };
    pthread_t thread;
    check(pthread_create(&thread, NULL, writer_main, &writer) == 0);

    const int64_t result = oreslang_source_actor_await_readable(
        actor, pipefd[0], resumed, UINT64_C(0x5151));
    check(result == 43);

    unsigned char byte = 0;
    check(read(pipefd[0], &byte, 1) == 1);
    check(byte == 0x41);
    check(pthread_join(thread, NULL) == 0);

    close(pipefd[0]);
    close(pipefd[1]);

    OresNativeExecutionHandle outside = {0, 0};
    check(!oreslang_source_actor_current_execution_handle(&outside));

    oreslang_source_actor_stop(actor);
    puts("SOURCE_ACTOR_READABLE_AWAIT_RUNTIME_PASS");
    return 0;
}
