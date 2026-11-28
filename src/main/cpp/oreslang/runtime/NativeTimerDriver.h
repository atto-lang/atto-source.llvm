#ifndef ORESLANG_RUNTIME_NATIVE_TIMER_DRIVER_H
#define ORESLANG_RUNTIME_NATIVE_TIMER_DRIVER_H

/*
 * One monotonic clock thread over one OresTimerWheel: the host arms a single
 * wakeup for the nearest logical deadline however many timers are outstanding
 * (contracts/v1/TIMER_ENGINE_V1.md). Mirrors the Java MonotonicTimerDriver.
 *
 * The driver waits on a condition variable until the wheel's next deadline
 * (CLOCK_MONOTONIC). Arming a timer earlier than the deadline the driver is
 * waiting for signals it under the same mutex, so no wakeup can be lost.
 * `fire` runs on the driver thread, outside the mutex, at most
 * ORES_TIMER_DRIVER_BATCH per wheel pass, in emission order. It must only
 * publish runtime state (e.g. enqueue a scheduler wake); never guest code.
 */

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>

#include "oreslang/runtime/NativeTimerWheel.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ORES_TIMER_DRIVER_BATCH 256

typedef void (*OresTimerFire)(void* ctx, uint64_t payload);

typedef struct OresTimerDriver {
    pthread_mutex_t lock;
    pthread_cond_t wake;
    pthread_t thread;
    OresTimerWheel wheel;
    OresTimerFire fire;
    void* ctx;
    uint64_t origin_ns;
    uint64_t waiting_until; /* guarded by lock; 0 while running a batch */
    bool stopping;
    bool started;
    OresTimerExpired batch[ORES_TIMER_DRIVER_BATCH];
} OresTimerDriver;

/* Monotonic nanoseconds from the host clock. */
uint64_t ores_timer_monotonic_ns(void);

/* Owner-serialized lifecycle: start only on fresh or fully stopped storage.
 * Publish the driver only after successful start. Quiesce external arm/cancel/
 * outstanding callers before stop; the fire callback must not call stop.
 * Handles are scoped to one successful start/stop lifetime. Failed start and
 * completed stop leave the driver inactive; operations on inactive or
 * zero-initialized storage return failure/zero without touching pthread state.
 * This contract does not authorize concurrent destruction or restart. */
bool ores_timer_driver_start(OresTimerDriver* d, uint32_t capacity, uint64_t tick_ns,
                             OresTimerFire fire, void* ctx);
/* Stops and joins the thread; pending timers are discarded, not fired. */
void ores_timer_driver_stop(OresTimerDriver* d);
/* Returns 0 when the wheel is full or the driver is stopping. */
uint64_t ores_timer_driver_arm_after(OresTimerDriver* d, uint64_t delay_ns, uint64_t payload);
bool ores_timer_driver_cancel(OresTimerDriver* d, uint64_t handle);
uint32_t ores_timer_driver_outstanding(OresTimerDriver* d);

#ifdef __cplusplus
}
#endif

#endif
