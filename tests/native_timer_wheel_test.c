/* Native timer engine tests (contracts/v1/TIMER_ENGINE_V1.md). The shared
 * conformance vector must match the Java backend's OresTimerWheelTest. */
#if !defined(__APPLE__) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L /* CLOCK_MONOTONIC, pthread_condattr_setclock, nanosleep */
#endif
#include "oreslang/runtime/NativeTimerDriver.h"
#include "oreslang/runtime/NativeTimerWheel.h"

#include <assert.h>
#include <inttypes.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MS UINT64_C(1000000)
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)

static OresTimerExpired out[4096];

static size_t poll1(OresTimerWheel* w, uint64_t now, size_t max) {
    return ores_timer_wheel_poll(w, now, out, max);
}

static void test_fires_at_deadline(void) {
    OresTimerWheel w;
    CHECK(ores_timer_wheel_init(&w, 8, MS, 0));
    ores_timer_wheel_arm(&w, 5 * MS, 1);
    CHECK(poll1(&w, 5 * MS - 1, 10) == 0);
    CHECK(poll1(&w, 5 * MS, 10) == 1 && out[0].payload == 1);
    CHECK(w.size == 0);
    ores_timer_wheel_destroy(&w);
}

static void test_sub_tick(void) {
    OresTimerWheel w;
    CHECK(ores_timer_wheel_init(&w, 8, MS, 0));
    ores_timer_wheel_arm(&w, MS + 900, 2);
    ores_timer_wheel_arm(&w, MS + 100, 1);
    CHECK(poll1(&w, MS + 99, 10) == 0);
    CHECK(poll1(&w, MS + 500, 10) == 1 && out[0].payload == 1);
    CHECK(ores_timer_wheel_next_deadline_ns(&w) == MS + 900);
    CHECK(poll1(&w, MS + 900, 10) == 1 && out[0].payload == 2);
    ores_timer_wheel_destroy(&w);
}

static void test_cascade_and_overflow(void) {
    OresTimerWheel w;
    CHECK(ores_timer_wheel_init(&w, 64, 1, 0));
    const uint64_t d[] = {1, 63, 64, 65, 4095, 4096, UINT64_C(1) << 18, (UINT64_C(1) << 24) + 7,
        (UINT64_C(1) << 30) - 1, (UINT64_C(1) << 35) + 3, (UINT64_C(1) << 36) - 1,
        UINT64_C(1) << 36, (UINT64_C(1) << 37) + 5, (UINT64_C(1) << 50) + 11};
    size_t n = sizeof d / sizeof d[0];
    for (size_t i = 0; i < n; i++) ores_timer_wheel_arm(&w, d[i], d[i]);
    for (size_t i = 0; i < n; i++) {
        CHECK(ores_timer_wheel_next_deadline_ns(&w) <= d[i]);
        CHECK(poll1(&w, d[i] - 1, 100) == 0);
        CHECK(poll1(&w, d[i], 100) == 1 && out[0].payload == d[i]);
    }
    CHECK(ores_timer_wheel_next_deadline_ns(&w) == ORES_TIMER_NO_DEADLINE);
    ores_timer_wheel_destroy(&w);
}

static void test_late_poll_cascade_order(void) {
    for (int k = 0; k < 2; k++) {
        uint64_t base = k ? UINT64_C(1) << 36 : 0;
        OresTimerWheel w;
        CHECK(ores_timer_wheel_init(&w, 8, 1, 0));
        CHECK(ores_timer_wheel_arm(&w, base + 127, 127));
        CHECK(ores_timer_wheel_arm(&w, base + 64, 64));
        CHECK(ores_timer_wheel_arm(&w, base + 65, 65));
        uint64_t cancelled = ores_timer_wheel_arm(&w, base + 65, 66);
        CHECK(cancelled);
        CHECK(poll1(&w, base + 63, 8) == 0);
        CHECK(poll1(&w, base + 127, 1) == 1 && out[0].payload == 64);
        CHECK(ores_timer_wheel_cancel(&w, cancelled));
        CHECK(poll1(&w, base + 127, 8) == 2 && out[0].payload == 65 && out[1].payload == 127);
        CHECK(w.size == 0);
        ores_timer_wheel_destroy(&w);
    }
    OresTimerWheel w;
    CHECK(ores_timer_wheel_init(&w, 8, 10, 0));
    CHECK(ores_timer_wheel_arm(&w, 649, 2));
    CHECK(ores_timer_wheel_arm(&w, 641, 1));
    CHECK(poll1(&w, 649, 8) == 2 && out[0].payload == 2 && out[1].payload == 1);
    ores_timer_wheel_destroy(&w);
}

static void test_stale_handles(void) {
    OresTimerWheel w;
    CHECK(ores_timer_wheel_init(&w, 1, MS, 0));
    uint64_t a = ores_timer_wheel_arm(&w, 10 * MS, 1);
    CHECK(ores_timer_wheel_cancel(&w, a));
    CHECK(!ores_timer_wheel_cancel(&w, a));
    uint64_t b = ores_timer_wheel_arm(&w, 10 * MS, 2);
    CHECK(a != b && !ores_timer_wheel_cancel(&w, a) && ores_timer_wheel_is_live(&w, b));
    CHECK(ores_timer_wheel_arm(&w, 1, 3) == 0); /* full */
    CHECK(poll1(&w, 10 * MS, 10) == 1 && out[0].payload == 2);
    CHECK(!ores_timer_wheel_cancel(&w, b) && !ores_timer_wheel_cancel(&w, 0)
          && !ores_timer_wheel_cancel(&w, UINT64_MAX));
    ores_timer_wheel_destroy(&w);
}

static void test_generation_exhaustion_and_reserved_deadline(void) {
    OresTimerWheel w;
    CHECK(ores_timer_wheel_init(&w, 2, MS, 0));
    CHECK(ores_timer_wheel_arm(&w, ORES_TIMER_NO_DEADLINE, 1) == 0);
    CHECK(w.size == 0 && ores_timer_wheel_next_deadline_ns(&w) == ORES_TIMER_NO_DEADLINE);
    uint64_t first = ores_timer_wheel_arm(&w, MS, 1);
    CHECK(ores_timer_wheel_cancel(&w, first));
    /* Seed the final generation instead of looping 2^32 times. */
    w.generation[0] = UINT32_MAX;
    uint64_t last = ores_timer_wheel_arm(&w, MS, 2);
    CHECK(last != first && ores_timer_wheel_cancel(&w, last));
    uint64_t sibling = ores_timer_wheel_arm(&w, MS, 3);
    CHECK(sibling != 0 && sibling != first && !ores_timer_wheel_cancel(&w, first));
    CHECK(ores_timer_wheel_is_live(&w, sibling));
    CHECK(ores_timer_wheel_cancel(&w, sibling));
    w.generation[1] = UINT32_MAX;
    uint64_t final = ores_timer_wheel_arm(&w, MS, 4);
    CHECK(final != 0 && poll1(&w, MS, 1) == 1 && out[0].handle == final);
    CHECK(!ores_timer_wheel_is_live(&w, final) && w.size == 0);
    CHECK(ores_timer_wheel_arm(&w, MS, 5) == 0);
    ores_timer_wheel_destroy(&w);
}

static void test_bounded_fifo(void) {
    OresTimerWheel w;
    CHECK(ores_timer_wheel_init(&w, 1024, MS, 0));
    uint64_t h[300];
    for (int i = 0; i < 300; i++) h[i] = ores_timer_wheel_arm(&w, 7 * MS, (uint64_t)i);
    CHECK(poll1(&w, 7 * MS, 100) == 100);
    for (int i = 0; i < 100; i++) CHECK(out[i].payload == (uint64_t)i);
    CHECK(ores_timer_wheel_cancel(&w, h[150]));
    size_t n = poll1(&w, 7 * MS, 1000);
    CHECK(n == 199 && out[0].payload == 100);
    for (size_t i = 0; i < n; i++) CHECK(out[i].payload != 150);
    ores_timer_wheel_destroy(&w);
}

static void test_clock_regression(void) {
    OresTimerWheel w;
    CHECK(ores_timer_wheel_init(&w, 4, MS, 100 * MS));
    ores_timer_wheel_arm(&w, 150 * MS, 9);
    CHECK(poll1(&w, 10 * MS, 10) == 0 && w.now_ns == 100 * MS);
    CHECK(poll1(&w, 150 * MS, 10) == 1);
    ores_timer_wheel_destroy(&w);
}

static void test_one_million(void) {
    const uint32_t n = 1000000;
    OresTimerWheel w;
    CHECK(ores_timer_wheel_init(&w, n, MS, 0));
    uint64_t x = 7;
    for (uint32_t i = 0; i < n; i++) {
        x = x * UINT64_C(6364136223846793005) + UINT64_C(1442695040888963407);
        CHECK(ores_timer_wheel_arm(&w, (x >> 11) % (600000 * MS), i) != 0);
    }
    uint64_t fired = 0;
    for (uint64_t now = 0; now <= 600000 * MS; now += 250 * MS) {
        size_t k;
        while ((k = poll1(&w, now, 4096)) > 0) {
            for (size_t i = 0; i < k; i++) CHECK(out[i].deadline_ns <= now);
            fired += k;
        }
    }
    CHECK(fired == n && w.size == 0);
    ores_timer_wheel_destroy(&w);
}

/* Identical algorithm to OresTimerWheelTest.TimerConformanceVector. */
static void test_conformance_vector(void) {
    static const char expected[] =
        "fired=11085 cancelled=793 full=0 polls=4074 checksum=cdc3f321e0578fc5";
    enum { STEPS = 20000 };
    static uint64_t handles[STEPS], deadlines[STEPS];
    OresTimerExpired o[64];
    OresTimerWheel w;
    CHECK(ores_timer_wheel_init(&w, 4096, MS, 0));
    uint64_t x = 0x5EED, now = 0, checksum = 0;
    unsigned next_id = 0, fired = 0, cancelled = 0, full = 0, polls = 0;
    for (int step = 0; step < STEPS; step++) {
        x = x * UINT64_C(6364136223846793005) + UINT64_C(1442695040888963407);
        uint64_t op = (x >> 33) % 10;
        x = x * UINT64_C(6364136223846793005) + UINT64_C(1442695040888963407);
        uint64_t r2 = x >> 33;
        if (op < 6) {
            uint64_t delay;
            switch (r2 % 4) {
                case 0: delay = (r2 >> 2) % UINT64_C(2000000); break;
                case 1: delay = (r2 >> 2) % UINT64_C(100000000); break;
                case 2: delay = (r2 >> 2) % UINT64_C(60000000000); break;
                default: delay = (r2 >> 2) * UINT64_C(4096); break;
            }
            unsigned id = next_id++;
            deadlines[id] = now + delay;
            uint64_t h = ores_timer_wheel_arm(&w, deadlines[id], id);
            if (h == 0) full++;
            handles[id] = h;
        } else if (op < 8) {
            if (next_id > 0) {
                unsigned id = (unsigned)(r2 % next_id);
                if (handles[id] != 0 && ores_timer_wheel_cancel(&w, handles[id])) cancelled++;
            }
        } else {
            now += (r2 % 97 == 0) ? (r2 % UINT64_C(1000000000000)) : (r2 % UINT64_C(50000000));
            size_t max = (size_t)(1 + (r2 >> 8) % 64);
            polls++;
            size_t n = ores_timer_wheel_poll(&w, now, o, max);
            for (size_t i = 0; i < n; i++) {
                CHECK(deadlines[o[i].payload] <= now);
                checksum = checksum * UINT64_C(1000003) + (o[i].payload ^ ((uint64_t)polls << 20));
                fired++;
            }
        }
    }
    now = (uint64_t)INT64_MAX / 2;
    size_t n;
    while ((n = ores_timer_wheel_poll(&w, now, o, 64)) > 0) {
        polls++;
        for (size_t i = 0; i < n; i++) {
            checksum = checksum * UINT64_C(1000003) + (o[i].payload ^ ((uint64_t)polls << 20));
            fired++;
        }
    }
    char got[160];
    snprintf(got, sizeof got, "fired=%u cancelled=%u full=%u polls=%u checksum=%" PRIx64,
             fired, cancelled, full, polls, checksum);
    if (strcmp(got, expected) != 0) {
        fprintf(stderr, "conformance vector mismatch\n  got      %s\n  expected %s\n", got, expected);
        exit(1);
    }
    printf("TIMER_CONFORMANCE_VECTOR_MATCH %s\n", got);
    ores_timer_wheel_destroy(&w);
}

/* ---- driver ---------------------------------------------------------- */

typedef struct { _Atomic uint64_t fired_mask; _Atomic uint64_t fired_at[8]; uint64_t origin; } Rec;

static void on_fire(void* ctx, uint64_t payload) {
    Rec* r = ctx;
    atomic_store(&r->fired_at[payload], ores_timer_monotonic_ns() - r->origin);
    atomic_fetch_or(&r->fired_mask, UINT64_C(1) << payload);
}

static void sleep_ms(long ms) {
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

static void test_driver(void) {
    Rec r;
    memset(&r, 0, sizeof r);
    r.origin = ores_timer_monotonic_ns();
    OresTimerDriver d;
    CHECK(ores_timer_driver_start(&d, 1024, MS, on_fire, &r));
    /* The driver first parks for an hour; an earlier arm must wake it. */
    uint64_t far = ores_timer_driver_arm_after(&d, 3600ull * 1000 * MS, 0);
    sleep_ms(20);
    ores_timer_driver_arm_after(&d, 30 * MS, 1);
    uint64_t cancelled = ores_timer_driver_arm_after(&d, 40 * MS, 2);
    CHECK(ores_timer_driver_cancel(&d, cancelled));
    for (int i = 0; i < 200 && !(atomic_load(&r.fired_mask) & 2); i++) sleep_ms(5);
    CHECK(atomic_load(&r.fired_mask) == 2);
    uint64_t at = atomic_load(&r.fired_at[1]);
    CHECK(at >= 50 * MS);  /* 20 ms sleep + 30 ms delay: never early */
    CHECK(at < 1000 * MS); /* and not lost behind the hour-long wait */
    sleep_ms(60);
    CHECK(atomic_load(&r.fired_mask) == 2); /* cancelled timer never fires */
    CHECK(ores_timer_driver_outstanding(&d) == 1 && ores_timer_driver_cancel(&d, far));
    ores_timer_driver_stop(&d);
    CHECK(ores_timer_driver_arm_after(&d, MS, 3) == 0);
    CHECK(!ores_timer_driver_cancel(&d, far));
    CHECK(ores_timer_driver_outstanding(&d) == 0);
    ores_timer_driver_stop(&d);
}

static void test_driver_inactive(void) {
    OresTimerDriver d = {0};
    CHECK(!ores_timer_driver_start(&d, 8, MS, NULL, NULL));
    CHECK(ores_timer_driver_arm_after(&d, MS, 0) == 0);
    CHECK(!ores_timer_driver_cancel(&d, 1));
    CHECK(ores_timer_driver_outstanding(&d) == 0);
    ores_timer_driver_stop(&d);
    CHECK(ores_timer_driver_arm_after(NULL, MS, 0) == 0);
    CHECK(!ores_timer_driver_cancel(NULL, 1));
    CHECK(ores_timer_driver_outstanding(NULL) == 0);
    ores_timer_driver_stop(NULL);
    Rec r = {0};
    r.origin = ores_timer_monotonic_ns();
    CHECK(ores_timer_driver_start(&d, 8, MS, on_fire, &r));
    CHECK(ores_timer_driver_arm_after(&d, 3600ull * 1000 * MS, 0) != 0);
    ores_timer_driver_stop(&d);
    CHECK(atomic_load(&r.fired_mask) == 0);
}

int main(void) {
    test_fires_at_deadline();
    test_sub_tick();
    test_cascade_and_overflow();
    test_late_poll_cascade_order();
    test_stale_handles();
    test_generation_exhaustion_and_reserved_deadline();
    test_bounded_fifo();
    test_clock_regression();
    test_one_million();
    test_conformance_vector();
    test_driver();
    test_driver_inactive();
    puts("native timer wheel: all tests passed");
    return 0;
}
