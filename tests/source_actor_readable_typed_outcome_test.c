#include "oreslang/runtime/SourceActorBootstrap.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

static atomic_int continuation_calls;
static atomic_int message_calls;
static void check(int ok) { if (!ok) abort(); }

static void actor_init(OresActorI64Heap* heap, uint64_t domain, uint64_t epoch) {
    (void)heap; check(domain != 0 && epoch != 0);
}
static int64_t actor_message(
        OresActorI64Heap* heap, uint64_t domain, uint64_t epoch, int64_t payload) {
    (void)heap; check(domain != 0 && epoch != 0);
    atomic_fetch_add_explicit(&message_calls, 1, memory_order_acq_rel);
    return payload;
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

typedef struct {
    void* actor;
    int fd;
    uint64_t token;
    OresSourceWritableAwaitOutcome outcome;
    int64_t result;
} WritableAwaitThread;

static void* await_main(void* opaque) {
    AwaitThread* await = (AwaitThread*)opaque;
    await->result = INT64_C(-999);
    await->outcome = oreslang_source_actor_await_readable_checked(
        await->actor, await->fd, resumed, await->token, &await->result);
    return NULL;
}

static void* writable_await_main(void* opaque) {
    WritableAwaitThread* await = (WritableAwaitThread*)opaque;
    await->result = INT64_C(-999);
    await->outcome = oreslang_source_actor_await_writable_checked(
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
static int make_write_nonblocking(int pipefd[2]) {
    const int flags = fcntl(pipefd[1], F_GETFL);
    return flags >= 0
        && fcntl(pipefd[1], F_SETFL, flags | O_NONBLOCK) == 0;
}
static size_t fill_pipe_until_would_block(int fd) {
    unsigned char bytes[4096];
    for (size_t i = 0; i < sizeof(bytes); ++i) bytes[i] = (unsigned char)(i & 0xffu);
    size_t total = 0;
    for (;;) {
        const ssize_t count = write(fd, bytes, sizeof(bytes));
        if (count > 0) {
            total += (size_t)count;
            continue;
        }
        if (count < 0 && errno == EINTR) continue;
        if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
        abort();
    }
    check(total > 0);
    return total;
}
static void drain_exact(int fd, size_t bytes) {
    unsigned char buffer[4096];
    size_t remaining = bytes;
    while (remaining > 0) {
        const size_t wanted = remaining < sizeof(buffer) ? remaining : sizeof(buffer);
        const ssize_t count = read(fd, buffer, wanted);
        if (count > 0) {
            remaining -= (size_t)count;
            continue;
        }
        if (count < 0 && errno == EINTR) continue;
        abort();
    }
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

static void run_stalled_read_allows_unrelated_actor_progress(void) {
    int pipefd[2] = {-1, -1}; check(make_pipe(pipefd, 1));
    void* waiting_actor = oreslang_source_actor_spawn(actor_init, actor_message, 1);
    void* progress_actor = oreslang_source_actor_spawn(actor_init, actor_message, 1);
    check(waiting_actor != NULL && progress_actor != NULL);
    reset_calls();
    atomic_store_explicit(&message_calls, 0, memory_order_release);

    AwaitThread await = {
        .actor = waiting_actor,
        .fd = pipefd[0],
        .token = UINT64_C(0x6180)
    };
    pthread_t thread; check(pthread_create(&thread, NULL, await_main, &await) == 0);

    /*
     * Wait until the exact one-shot watch has been published. From this point
     * the await thread has left the actor lane and is entering/already inside
     * the trusted outer-host completion-fd wait.
     */
    const struct timespec pause = { .tv_sec = 0, .tv_nsec = 1000000 };
    int watch_admitted = 0;
    for (int i = 0; i < 3000; ++i) {
        if (oreslang_source_actor_test_readable_await_admitted()) {
            watch_admitted = 1;
            break;
        }
        check(nanosleep(&pause, NULL) == 0);
    }
    check(watch_admitted);

    /*
     * The trusted host wait must consume no source-actor carrier: unrelated
     * DATA must still run while this silent readable await remains pending.
     */
    oreslang_source_actor_send(progress_actor, INT64_C(77));
    int progressed = 0;
    for (int i = 0; i < 3000; ++i) {
        if (atomic_load_explicit(&message_calls, memory_order_acquire) != 0) {
            progressed = 1;
            break;
        }
        check(nanosleep(&pause, NULL) == 0);
    }
    check(progressed);
    check(atomic_load_explicit(&continuation_calls, memory_order_acquire) == 0);

    wait_for_cancel_success();
    check(pthread_join(thread, NULL) == 0);
    check(await.outcome == ORES_SOURCE_READABLE_AWAIT_CANCELLED);
    check(await.result == 0);
    check(atomic_load_explicit(&continuation_calls, memory_order_acquire) == 0);

    close_pipe(pipefd);
    oreslang_source_actor_stop(progress_actor);
    oreslang_source_actor_stop(waiting_actor);
}

static void run_cancel_admission_race_stress(void) {
    /*
     * Repeatedly race the trusted cancellation hook with watch admission.
     * The hook reads frame.admitted under the source-group mutex, so this is
     * also a TSan regression for publication of the admitted watch identity.
     * Cancellation must never execute the compiled continuation or consume
     * pipe data.
     */
    for (uint64_t i = 0; i < UINT64_C(64); ++i) {
        int pipefd[2] = {-1, -1}; check(make_pipe(pipefd, 1));
        void* actor = oreslang_source_actor_spawn(actor_init, actor_message, 1);
        check(actor != NULL); reset_calls();
        AwaitThread await = {
            .actor = actor,
            .fd = pipefd[0],
            .token = UINT64_C(0x6200) + i
        };
        pthread_t thread; check(pthread_create(&thread, NULL, await_main, &await) == 0);
        wait_for_cancel_success();
        check(pthread_join(thread, NULL) == 0);
        check(await.outcome == ORES_SOURCE_READABLE_AWAIT_CANCELLED);
        check(await.result == 0);
        check(atomic_load_explicit(&continuation_calls, memory_order_acquire) == 0);

        /* The cancelled watch must not consume later input. */
        const unsigned char byte = (unsigned char)(0x40u + (unsigned)i % 32u);
        check(write(pipefd[1], &byte, 1) == 1);
        unsigned char observed = 0;
        check(read(pipefd[0], &observed, 1) == 1);
        check(observed == byte);

        close_pipe(pipefd);
        oreslang_source_actor_stop(actor);
    }
}

static void run_writable_ready(void) {
    int pipefd[2] = {-1, -1};
    check(make_pipe(pipefd, 0));
    check(make_write_nonblocking(pipefd));
    void* actor = oreslang_source_actor_spawn(actor_init, actor_message, 1);
    check(actor != NULL);
    reset_calls();

    int64_t result = INT64_C(-999);
    const OresSourceWritableAwaitOutcome outcome =
        oreslang_source_actor_await_writable_checked(
            actor, pipefd[1], resumed, UINT64_C(0x6301), &result);
    check(outcome == ORES_SOURCE_WRITABLE_AWAIT_READY);
    check(result == 43);
    check(atomic_load_explicit(&continuation_calls, memory_order_acquire) == 1);

    close_pipe(pipefd);
    oreslang_source_actor_stop(actor);
}

static void run_stalled_writable_allows_progress_cancel_and_rearm(void) {
    int pipefd[2] = {-1, -1};
    check(make_pipe(pipefd, 0));
    check(make_write_nonblocking(pipefd));
    const size_t filled = fill_pipe_until_would_block(pipefd[1]);

    void* waiting_actor = oreslang_source_actor_spawn(actor_init, actor_message, 1);
    void* progress_actor = oreslang_source_actor_spawn(actor_init, actor_message, 1);
    check(waiting_actor != NULL && progress_actor != NULL);
    reset_calls();
    atomic_store_explicit(&message_calls, 0, memory_order_release);

    WritableAwaitThread await = {
        .actor = waiting_actor,
        .fd = pipefd[1],
        .token = UINT64_C(0x6302)
    };
    pthread_t thread;
    check(pthread_create(&thread, NULL, writable_await_main, &await) == 0);

    const struct timespec pause = { .tv_sec = 0, .tv_nsec = 1000000 };
    int admitted = 0;
    for (int i = 0; i < 3000; ++i) {
        if (oreslang_source_actor_test_io_await_admitted()) {
            admitted = 1;
            break;
        }
        check(nanosleep(&pause, NULL) == 0);
    }
    check(admitted);

    oreslang_source_actor_send(progress_actor, INT64_C(88));
    int progressed = 0;
    for (int i = 0; i < 3000; ++i) {
        if (atomic_load_explicit(&message_calls, memory_order_acquire) != 0) {
            progressed = 1;
            break;
        }
        check(nanosleep(&pause, NULL) == 0);
    }
    check(progressed);
    check(atomic_load_explicit(&continuation_calls, memory_order_acquire) == 0);

    check(oreslang_source_actor_test_cancel_io_await());
    check(pthread_join(thread, NULL) == 0);
    check(await.outcome == ORES_SOURCE_WRITABLE_AWAIT_CANCELLED);
    check(await.result == 0);
    check(atomic_load_explicit(&continuation_calls, memory_order_acquire) == 0);

    /*
     * Physically restore pipe capacity and reuse the exact write descriptor.
     * The cancelled one-shot EPOLLOUT watch must have retired, otherwise this
     * fresh generation cannot own and settle the same fd.
     */
    drain_exact(pipefd[0], filled);
    reset_calls();
    int64_t rearmed_result = INT64_C(-999);
    const OresSourceWritableAwaitOutcome rearmed =
        oreslang_source_actor_await_writable_checked(
            waiting_actor, pipefd[1], resumed, UINT64_C(0x6303),
            &rearmed_result);
    check(rearmed == ORES_SOURCE_WRITABLE_AWAIT_READY);
    check(rearmed_result == 43);
    check(atomic_load_explicit(&continuation_calls, memory_order_acquire) == 1);

    close_pipe(pipefd);
    oreslang_source_actor_stop(progress_actor);
    oreslang_source_actor_stop(waiting_actor);
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
    run_stalled_read_allows_unrelated_actor_progress();
    run_cancel_admission_race_stress();
    run_writable_ready();
    run_stalled_writable_allows_progress_cancel_and_rearm();
    run_terminated();
    run_failed_admission();
    puts("SOURCE_ACTOR_READABLE_TYPED_OUTCOME_PASS");
    return 0;
}
