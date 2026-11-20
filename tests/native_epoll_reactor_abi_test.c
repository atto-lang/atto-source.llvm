#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "oreslang/runtime/NativeEpollReactorABI.h"
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include <poll.h>
#define require(condition) do { if (!(condition)) { \
    fprintf(stderr, "require failed at line %d: %s\n", __LINE__, #condition); \
    abort(); \
} } while (0)
/* try_* deliberately rejects lock contention without parking a carrier.
 * Only the host test retries; invalid/stale operations below remain single
 * attempts and must reject. No runtime retry or blocking is introduced. */
static int watch_eventually(OresNativeNio* runtime, int fd, OresNativeNioWatch* out) {
    const struct timespec delay = {0, 1000000};
    for (int i = 0; i < 3000; ++i) {
        if (ores_native_nio_try_watch(runtime, fd, EPOLLIN, out)) return 1;
        nanosleep(&delay, NULL);
    }
    return 0;
}
static int cancel_eventually(OresNativeNio* runtime, OresNativeNioWatch watch) {
    const struct timespec delay = {0, 1000000};
    for (int i = 0; i < 3000; ++i) {
        if (ores_native_nio_try_cancel(runtime, watch)) return 1;
        nanosleep(&delay, NULL);
    }
    return 0;
}
static int next_completion(OresNativeNio* runtime, OresNativeNioCompletion* out) {
    struct timespec delay = {0, 1000000};
    for (int i = 0; i < 3000; ++i) {
        if (ores_native_nio_try_pop(runtime, out)) return 1;
        nanosleep(&delay, NULL); /* test-only host timeout */
    }
    return 0;
}
int main(void) {
    OresNativeNio* runtime = ores_native_nio_open(4, 0, 1);
    require(runtime && ores_native_nio_started_reactors(runtime) == 0);
    require(ores_native_nio_completion_fd(runtime, 0) == -1);
    // epoll readiness cannot make a blocking descriptor safe for guest I/O.
    int blocking[2] = {-1, -1};
    require(pipe(blocking) == 0);
    OresNativeNioWatch forbidden = {0};
    require(!ores_native_nio_try_watch(runtime, blocking[0], EPOLLIN, &forbidden));
    require(forbidden.ticket == 0);
    require(ores_native_nio_started_reactors(runtime) == 0);
    close(blocking[0]); close(blocking[1]);
    int pipefd[2] = {-1, -1};
    require(pipe2(pipefd, O_NONBLOCK | O_CLOEXEC) == 0);
    OresNativeNioWatch first = {0};
    require(watch_eventually(runtime, pipefd[0], &first));
    require(first.ticket != 0 && first.reactor == 0);
    require(ores_native_nio_started_reactors(runtime) == 1);
    require(ores_native_nio_completion_fd(runtime, 0) >= 0);
    require(write(pipefd[1], "!", 1) == 1);
    /* A host polling the completion FD must be able to acknowledge its
     * readiness without entering a permanently readable busy loop. */
    struct pollfd notification = {ores_native_nio_completion_fd(runtime, 0), POLLIN, 0};
    require(poll(&notification, 1, 1000) == 1);
    ores_native_nio_drain_completion_signal(runtime, 0);
    // Acknowledging before consuming an outstanding completion must re-arm
    // readiness. Otherwise an outer poller could sleep indefinitely after
    // a temporary nonparking try_pop lock miss.
    notification.revents = 0;
    require(poll(&notification, 1, 0) == 1);
    OresNativeNioCompletion received = {0};
    require(next_completion(runtime, &received));
    ores_native_nio_drain_completion_signal(runtime, 0);
    notification.revents = 0;
    require(poll(&notification, 1, 0) == 0);
    require(received.outcome == ORES_NATIVE_NIO_READY);
    require(received.watch.ticket == first.ticket);
    require(received.fd == pipefd[0] && (received.events & EPOLLIN));
    char c;
    require(read(pipefd[0], &c, 1) == 1 && c == '!');
    OresNativeNioWatch second = {0};
    require(watch_eventually(runtime, pipefd[0], &second));
    require(second.ticket != first.ticket);
    require(!ores_native_nio_try_cancel(runtime, first));
    require(cancel_eventually(runtime, second));
    require(!ores_native_nio_try_cancel(runtime, second));
    notification.revents = 0;
    require(poll(&notification, 1, 1000) == 1);
    ores_native_nio_drain_completion_signal(runtime, 0);
    notification.revents = 0;
    require(poll(&notification, 1, 0) == 1);
    require(next_completion(runtime, &received));
    ores_native_nio_drain_completion_signal(runtime, 0);
    notification.revents = 0;
    require(poll(&notification, 1, 0) == 0);
    require(received.outcome == ORES_NATIVE_NIO_CANCELLED);
    require(received.watch.ticket == second.ticket);
    close(pipefd[0]); close(pipefd[1]);
    ores_native_nio_close(runtime);

    OresNativeNio* realtime = ores_native_nio_open(3, 1, 3);
    require(realtime != NULL && ores_native_nio_started_reactors(realtime) == 3);
    ores_native_nio_close(realtime);
    require(ores_native_nio_open(5, 1, 5) == NULL);
    return 0;
}
