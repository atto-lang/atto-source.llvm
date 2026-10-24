#pragma once
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* Host-only Linux NIO readiness ABI. No guest callbacks, borrowed actor
 * pointers, synchronous socket I/O, heap allocations on the hot path, or
 * continuation execution. Caller retains each watched FD until completion.
 * The return status of try_* is 1 for success, 0 for unavailable/rejected.
 */
typedef struct OresNativeNio OresNativeNio;
typedef struct {
    uint64_t ticket;
    uint8_t reactor;
} OresNativeNioWatch;
typedef enum {
    ORES_NATIVE_NIO_READY = 1,
    ORES_NATIVE_NIO_CANCELLED = 2,
    ORES_NATIVE_NIO_STOPPED = 3
} OresNativeNioOutcome;
typedef struct {
    OresNativeNioWatch watch;
    int fd;
    uint32_t events;
    OresNativeNioOutcome outcome;
} OresNativeNioCompletion;

/* Creates bounded selector resources before guest admission. Normal
 * throughput starts zero reactor pthreads; the first valid try_watch lazily
 * starts reactor #0. realtime_preallocated=1 eagerly prestarts configured
 * rt_reactors before returning; kernel/OS WCET is NOT guaranteed.
 */
OresNativeNio* ores_native_nio_open(
    size_t max_reactors, int realtime_preallocated, size_t rt_reactors);
size_t ores_native_nio_started_reactors(const OresNativeNio* manager);

/* Linux EPOLLIN/EPOLLOUT flags are passed verbatim; other ops fail closed.
 * An FD without O_NONBLOCK fails admission. After successful admission the
 * caller MUST keep that FD alive, nonblocking, and under the same ownership
 * until its terminal completion is consumed; readiness does not itself
 * guarantee subsequent read/write syscalls are nonblocking.
 * Admission is nonparking and bounded by 64 outstanding tickets/reactor.
 */
int ores_native_nio_try_watch(
    OresNativeNio* manager, int fd, uint32_t events, OresNativeNioWatch* output);
int ores_native_nio_try_cancel(OresNativeNio* manager, OresNativeNioWatch watch);
int ores_native_nio_try_pop(OresNativeNio* manager, OresNativeNioCompletion* output);
int ores_native_nio_completion_fd(const OresNativeNio* manager, size_t reactor);

/* A host POLLIN observer acknowledges the eventfd before trying to pop.
 * The counter is a wakeup signal, not the number of completed operations.
 * If records are still queued when acknowledged (including because a
 * nonparking try_pop loses lock contention), readiness is re-armed. A host
 * must continue popping until empty; a single harmless extra wakeup can
 * occur when a different consumer empties the queue concurrently.
 * No waits or guest callbacks.
 */
void ores_native_nio_drain_completion_signal(OresNativeNio* manager, size_t reactor);

/* External trusted host ONLY: close joins I/O pthreads, never call on an
 * actor, ROOT/CONTROL carrier or producer concurrently using this object.
 * Every producer must already be fenced before destroy.
 */
void ores_native_nio_close(OresNativeNio* manager);

#ifdef __cplusplus
}
#endif
