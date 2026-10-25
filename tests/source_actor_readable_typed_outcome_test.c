#include "oreslang/runtime/SourceActorBootstrap.h"

#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

static atomic_int continuation_calls;
static void check(int ok) { if (!ok) abort(); }

static void actor_init(OresActorI64Heap* heap, uint64_t domain, uint64_t epoch) {
    (void)heap; check(domain != 0 && epoch != 0);
}
static int64_t actor_message(
        OresActorI64Heap* heap, uint64_t domain, uint64_t epoch, int64_t payload) {
    (void)heap; check(domain != 0 && epoch != 0); return payload;
}
static int64_t resumed(void* actor) {
    (void)actor;
    atomic_fetch_add_explicit(&continuation_calls, 1, memory_order_acq_rel);
    return 43;
}

typedef struct {
    void* actor;
    int fd;
    uint64_t token;
    OresSourceReadableAwaitOutcome outcome;
    int64_t result;
} AwaitThread;

static void* await_main(void* opaque) {
    AwaitThread* await = (AwaitThread*)opaque;
    await->result = INT64_C(-999);
    await->outcome = oreslang_source_actor_await_readable_checked(
        await->actor, await->fd, resumed, await->token, &await->result);
    return NULL;
}

static int make_pipe(int pipefd[2], int nonblocking_read) {
    if (pipe(pipefd) != 0) return 0;
    if (nonblocking_read) {
        const int flags = fcntl(pipefd[0], F_GETFL);
        if (flags < 0 || fcntl(pipefd[0], F_SETFL, flags | O_NONBLOCK) != 0) {
            close(pipefd[0]); close(pipefd[1]); return 0;
        }
    }
    return 1;
}
static void close_pipe(int pipefd[2]) { close(pipefd[0]); close(pipefd[1]); }
static void reset_calls(void) {
    atomic_store_explicit(&continuation_calls, 0, memory_order_release);
}
static void wait_for_cancel_success(void) {
    const struct timespec pause = { .tv_sec = 0, .tv_nsec = 1000000 };
    for (int i = 0; i < 3000; ++i) {
        if (oreslang_source_actor_test_cancel_readable_await()) return;
        check(nanosleep(&pause, NULL) == 0);
    }
    abort();
}
static void wait_for_root_stop_success(void) {
    const struct timespec pause = { .tv_sec = 0, .tv_nsec = 1000000 };
    for (int i = 0; i < 3000; ++i) {
        if (oreslang_source_actor_test_stop_readable_root()) return;
        check(nanosleep(&pause, NULL) == 0);
    }
    abort();
}

static void run_ready(void) {
    int pipefd[2] = {-1, -1}; check(make_pipe(pipefd, 1));
    void* actor = oreslang_source_actor_spawn(actor_init, actor_message, 1);
    check(actor != NULL); reset_calls();
    AwaitThread await = { .actor = actor, .fd = pipefd[0], .token = UINT64_C(0x6101) };
    pthread_t thread; check(pthread_create(&thread, NULL, await_main, &await) == 0);
    const struct timespec pause = { .tv_sec = 0, .tv_nsec = 20 * 1000 * 1000 };
    check(nanosleep(&pause, NULL) == 0);
    const unsigned char byte = 0x52; check(write(pipefd[1], &byte, 1) == 1);
    check(pthread_join(thread, NULL) == 0);
    check(await.outcome == ORES_SOURCE_READABLE_AWAIT_READY);
    check(await.result == 43);
    check(atomic_load_explicit(&continuation_calls, memory_order_acquire) == 1);
    close_pipe(pipefd); oreslang_source_actor_stop(actor);
}

static void run_cancelled(void) {
    int pipefd[2] = {-1, -1}; check(make_pipe(pipefd, 1));
    void* actor = oreslang_source_actor_spawn(actor_init, actor_message, 1);
    check(actor != NULL); reset_calls();
    AwaitThread await = { .actor = actor, .fd = pipefd[0], .token = UINT64_C(0x6102) };
    pthread_t thread; check(pthread_create(&thread, NULL, await_main, &await) == 0);
    wait_for_cancel_success();
    check(pthread_join(thread, NULL) == 0);
    check(await.outcome == ORES_SOURCE_READABLE_AWAIT_CANCELLED);
    check(await.result == 0);
    check(atomic_load_explicit(&continuation_calls, memory_order_acquire) == 0);
    close_pipe(pipefd); oreslang_source_actor_stop(actor);
}

static void run_terminated(void) {
    int pipefd[2] = {-1, -1}; check(make_pipe(pipefd, 1));
    void* actor = oreslang_source_actor_spawn(actor_init, actor_message, 1);
    check(actor != NULL); reset_calls();
    AwaitThread await = { .actor = actor, .fd = pipefd[0], .token = UINT64_C(0x6103) };
    pthread_t thread; check(pthread_create(&thread, NULL, await_main, &await) == 0);
    wait_for_root_stop_success();
    wait_for_cancel_success();
    check(pthread_join(thread, NULL) == 0);
    check(await.outcome == ORES_SOURCE_READABLE_AWAIT_TERMINATED);
    check(await.result == 0);
    check(atomic_load_explicit(&continuation_calls, memory_order_acquire) == 0);
    close_pipe(pipefd); oreslang_source_actor_stop(actor);
}

static void run_failed_admission(void) {
    int pipefd[2] = {-1, -1}; check(make_pipe(pipefd, 0));
    void* actor = oreslang_source_actor_spawn(actor_init, actor_message, 1);
    check(actor != NULL); reset_calls();
    int64_t result = INT64_C(-999);
    const OresSourceReadableAwaitOutcome outcome =
        oreslang_source_actor_await_readable_checked(
            actor, pipefd[0], resumed, UINT64_C(0x6104), &result);
    check(outcome == ORES_SOURCE_READABLE_AWAIT_FAILED);
    check(result == 0);
    check(atomic_load_explicit(&continuation_calls, memory_order_acquire) == 0);
    close_pipe(pipefd); oreslang_source_actor_stop(actor);
}

int main(void) {
    run_ready();
    run_cancelled();
    run_terminated();
    run_failed_admission();
    puts("SOURCE_ACTOR_READABLE_TYPED_OUTCOME_PASS");
    return 0;
}
