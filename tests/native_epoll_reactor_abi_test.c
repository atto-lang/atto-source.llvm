#define _GNU_SOURCE
#include "oreslang/runtime/NativeEpollReactorABI.h"
#include <stdlib.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <poll.h>
static void require(int condition) { if (!condition) abort(); }
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
    require(runtime != NULL);
    /* Throughput mode must remain cold until a VALID watch is admitted. */
    require(ores_native_nio_started_reactors(runtime) == 0);
    require(ores_native_nio_completion_fd(runtime, 0) == -1);
    // epoll readiness cannot make a blocking descriptor safe for guest I/O.
    int blocking[2] = {-1, -1};
    require(pipe(blocking) == 0);
    OresNativeNioWatch forbidden = {0};
    require(!ores_native_nio_try_watch(runtime, blocking[0], EPOLLIN, &forbidden));
    require(forbidden.ticket == 0);
    require(ores_native_nio_started_reactors(runtime) == 0);
    require(ores_native_nio_completion_fd(runtime, 0) == -1);
    close(blocking[0]); close(blocking[1]);
    /* A nonblocking but unauthenticated anonymous FD must also fail closed
     * without provisioning any reactor worker or publishing a token. */
    int raw_event = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    require(raw_event >= 0);
    require(!ores_native_nio_try_watch(runtime, raw_event, EPOLLIN, &forbidden));
    require(forbidden.ticket == 0);
    require(ores_native_nio_started_reactors(runtime) == 0);
    require(ores_native_nio_completion_fd(runtime, 0) == -1);
    close(raw_event);
    int pipefd[2] = {-1, -1};
    require(pipe2(pipefd, O_NONBLOCK | O_CLOEXEC) == 0);
    OresNativeNioWatch first = {0};
    require(ores_native_nio_try_watch(runtime, pipefd[0], EPOLLIN, &first));
    require(first.ticket != 0 && first.reactor == 0);
    require(ores_native_nio_started_reactors(runtime) == 1);
    require(ores_native_nio_completion_fd(runtime, 0) >= 0);
    /* Foreign runtime handles must not collide, even when both reactors
     * start at slot 1 / generation 1. Public ABI size remains unchanged. */
    OresNativeNio* foreign_runtime = ores_native_nio_open(1, 1, 1);
    require(foreign_runtime != NULL);
    int foreign_pipe[2] = {-1, -1};
    require(pipe2(foreign_pipe, O_NONBLOCK | O_CLOEXEC) == 0);
    OresNativeNioWatch foreign_watch = {0};
    require(ores_native_nio_try_watch(
        foreign_runtime, foreign_pipe[0], EPOLLIN, &foreign_watch));
    require(foreign_watch.ticket != first.ticket);
    require(!ores_native_nio_try_cancel(foreign_runtime, first));
    require(!ores_native_nio_try_cancel(runtime, foreign_watch));
    require(ores_native_nio_try_cancel(foreign_runtime, foreign_watch));
    OresNativeNioCompletion foreign_result = {0};
    require(next_completion(foreign_runtime, &foreign_result));
    require(foreign_result.outcome == ORES_NATIVE_NIO_CANCELLED);
    require(foreign_result.watch.ticket == foreign_watch.ticket);
    close(foreign_pipe[0]); close(foreign_pipe[1]);
    ores_native_nio_close(foreign_runtime);
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
    require(ores_native_nio_try_watch(runtime, pipefd[0], EPOLLIN, &second));
    require(second.ticket != first.ticket);
    require(!ores_native_nio_try_cancel(runtime, first));
    require(ores_native_nio_try_cancel(runtime, second));
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
