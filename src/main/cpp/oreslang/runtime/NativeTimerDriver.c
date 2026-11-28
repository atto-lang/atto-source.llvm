#if !defined(__APPLE__) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L /* CLOCK_MONOTONIC, pthread_condattr_setclock, nanosleep */
#endif
#include "oreslang/runtime/NativeTimerDriver.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

uint64_t ores_timer_monotonic_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * UINT64_C(1000000000) + (uint64_t)ts.tv_nsec;
}

static uint64_t driver_now(const OresTimerDriver* d) {
    uint64_t t = ores_timer_monotonic_ns();
    return t > d->origin_ns ? t - d->origin_ns : 0;
}

/* Wait on d->wake until relative timeout elapses or a signal arrives. */
static void timed_wait(OresTimerDriver* d, uint64_t wait_ns) {
#if defined(__APPLE__)
    struct timespec rel = { (time_t)(wait_ns / UINT64_C(1000000000)),
                            (long)(wait_ns % UINT64_C(1000000000)) };
    (void)pthread_cond_timedwait_relative_np(&d->wake, &d->lock, &rel);
#else
    uint64_t abs_ns = ores_timer_monotonic_ns();
    abs_ns = wait_ns > UINT64_MAX - abs_ns ? UINT64_MAX : abs_ns + wait_ns;
    struct timespec ts = { (time_t)(abs_ns / UINT64_C(1000000000)),
                           (long)(abs_ns % UINT64_C(1000000000)) };
    (void)pthread_cond_timedwait(&d->wake, &d->lock, &ts);
#endif
}

static void* driver_main(void* raw) {
    OresTimerDriver* d = raw;
    pthread_mutex_lock(&d->lock);
    while (!d->stopping) {
        size_t fired = ores_timer_wheel_poll(&d->wheel, driver_now(d), d->batch,
                                             ORES_TIMER_DRIVER_BATCH);
        if (fired > 0) {
            d->waiting_until = 0;
            pthread_mutex_unlock(&d->lock);
            for (size_t i = 0; i < fired; i++) d->fire(d->ctx, d->batch[i].payload);
            pthread_mutex_lock(&d->lock);
            continue;
        }
        uint64_t next = ores_timer_wheel_next_deadline_ns(&d->wheel);
        d->waiting_until = next;
        if (next == ORES_TIMER_NO_DEADLINE) {
            pthread_cond_wait(&d->wake, &d->lock);
        } else {
            uint64_t now = driver_now(d);
            if (next > now) timed_wait(d, next - now);
        }
    }
    pthread_mutex_unlock(&d->lock);
    return NULL;
}

bool ores_timer_driver_start(OresTimerDriver* d, uint32_t capacity, uint64_t tick_ns,
                             OresTimerFire fire, void* ctx) {
    if (!d) return false;
    memset(d, 0, sizeof(*d));
    if (!fire) return false;
    if (!ores_timer_wheel_init(&d->wheel, capacity, tick_ns, 0)) return false;
    d->fire = fire;
    d->ctx = ctx;
    d->origin_ns = ores_timer_monotonic_ns();
    pthread_condattr_t attr;
    if (pthread_condattr_init(&attr) != 0) {
        ores_timer_wheel_destroy(&d->wheel);
        return false;
    }
#if !defined(__APPLE__)
    if (pthread_condattr_setclock(&attr, CLOCK_MONOTONIC) != 0) {
        pthread_condattr_destroy(&attr);
        ores_timer_wheel_destroy(&d->wheel);
        return false;
    }
#endif
    if (pthread_mutex_init(&d->lock, NULL) != 0) {
        pthread_condattr_destroy(&attr);
        ores_timer_wheel_destroy(&d->wheel);
        return false;
    }
    if (pthread_cond_init(&d->wake, &attr) != 0) {
        pthread_mutex_destroy(&d->lock);
        pthread_condattr_destroy(&attr);
        ores_timer_wheel_destroy(&d->wheel);
        return false;
    }
    pthread_condattr_destroy(&attr);
    if (pthread_create(&d->thread, NULL, driver_main, d) != 0) {
        pthread_cond_destroy(&d->wake);
        pthread_mutex_destroy(&d->lock);
        ores_timer_wheel_destroy(&d->wheel);
        return false;
    }
    d->started = true;
    return true;
}

void ores_timer_driver_stop(OresTimerDriver* d) {
    if (!d || !d->started) return;
    pthread_mutex_lock(&d->lock);
    d->stopping = true;
    pthread_cond_signal(&d->wake);
    pthread_mutex_unlock(&d->lock);
    pthread_join(d->thread, NULL);
    pthread_cond_destroy(&d->wake);
    pthread_mutex_destroy(&d->lock);
    ores_timer_wheel_destroy(&d->wheel);
    d->started = false;
}

uint64_t ores_timer_driver_arm_after(OresTimerDriver* d, uint64_t delay_ns, uint64_t payload) {
    if (!d || !d->started) return 0;
    pthread_mutex_lock(&d->lock);
    uint64_t handle = 0;
    if (!d->stopping) {
        uint64_t now = driver_now(d);
        uint64_t deadline = delay_ns > UINT64_MAX - 1 - now ? UINT64_MAX - 1 : now + delay_ns;
        handle = ores_timer_wheel_arm(&d->wheel, deadline, payload);
        /* The driver publishes waiting_until under this lock before waiting,
         * so an earlier arm always reaches a waiting (or about-to-wait) driver. */
        if (handle != 0 && d->waiting_until != 0 && deadline < d->waiting_until)
            pthread_cond_signal(&d->wake);
    }
    pthread_mutex_unlock(&d->lock);
    return handle;
}

bool ores_timer_driver_cancel(OresTimerDriver* d, uint64_t handle) {
    if (!d || !d->started) return false;
    pthread_mutex_lock(&d->lock);
    bool ok = !d->stopping && ores_timer_wheel_cancel(&d->wheel, handle);
    pthread_mutex_unlock(&d->lock);
    return ok;
}

uint32_t ores_timer_driver_outstanding(OresTimerDriver* d) {
    if (!d || !d->started) return 0;
    pthread_mutex_lock(&d->lock);
    uint32_t n = d->wheel.size;
    pthread_mutex_unlock(&d->lock);
    return n;
}
