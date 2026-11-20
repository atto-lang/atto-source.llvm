#include "oreslang/runtime/NativeActorLogicalFramesV1.h"
#include <pthread.h>
#include <stdatomic.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "fail:%d %s\n", __LINE__, #c); abort(); } } while (0)

static _Atomic int simultaneous = 0;
typedef struct {
    OresNativeActorLogicalFramesV1* stack;
    int success;
} Worker;
static void* concurrent(void* ptr) {
    Worker* w = ptr;
    OresLogicalTurnV1 t = {0}; OresLogicalFrameV1 f = {0};
    uint32_t x = 42, y = 0;
    if (!ores_logical_frames_v1_begin(w->stack, 7, 1, &t)) return NULL;
    atomic_fetch_add_explicit(&simultaneous, 1, memory_order_release);
    while (atomic_load_explicit(&simultaneous, memory_order_acquire) != 2) sched_yield();
    w->success = ores_logical_frames_v1_push(w->stack, t, &f) &&
        ores_logical_frames_v1_write(w->stack, f, 0, &x, sizeof x) &&
        ores_logical_frames_v1_read(w->stack, f, 0, &y, sizeof y) && y == 42 &&
        ores_logical_frames_v1_pop(w->stack, f) &&
        ores_logical_frames_v1_end(w->stack, t);
    return NULL;
}
int main(void) {
    CHECK(!ores_logical_frames_v1_open(0, 1, 2, 2, 16));
    CHECK(!ores_logical_frames_v1_open(1, 1, 65, 2, 16));
    OresNativeActorLogicalFramesV1* a = ores_logical_frames_v1_open(7, 1, 2, 2, 16);
    OresNativeActorLogicalFramesV1* b = ores_logical_frames_v1_open(8, 1, 1, 1, 16);
    OresNativeActorLogicalFramesV1* duplicate = ores_logical_frames_v1_open(7, 1, 1, 1, 16);
    CHECK(a && b && duplicate);
    OresLogicalTurnV1 ta = {0}, tb = {0};
    OresLogicalFrameV1 f0 = {0}, f1 = {0}, fb = {0};
    CHECK(!ores_logical_frames_v1_begin(a, 8, 1, &ta));
    CHECK(ores_logical_frames_v1_begin(a, 7, 1, &ta));
    CHECK(!ores_logical_frames_v1_begin(a, 7, 1, &tb)); /* same carrier */
    CHECK(!ores_logical_frames_v1_push(duplicate, ta, &f0)); /* domain fence */
    CHECK(ores_logical_frames_v1_push(a, ta, &f0));
    CHECK(ores_logical_frames_v1_push(a, ta, &f1));
    CHECK(!ores_logical_frames_v1_push(a, ta, &fb)); /* quota actor-local */
    CHECK(!ores_logical_frames_v1_end(a, ta)); /* cannot leak frames */
    uint32_t data = 0xfeedbeef, out = 0;
    CHECK(ores_logical_frames_v1_write(a, f0, 0, &data, sizeof data));
    CHECK(ores_logical_frames_v1_read(a, f0, 0, &out, sizeof out) && out == data);
    CHECK(!ores_logical_frames_v1_read(a, f0, 15, &out, sizeof out));
    CHECK(!ores_logical_frames_v1_pop(a, f0)); /* LIFO */
    CHECK(ores_logical_frames_v1_cancel(a, ta));
    CHECK(!ores_logical_frames_v1_read(a, f0, 0, &out, sizeof out));
    CHECK(!ores_logical_frames_v1_pop(a, f1));
    CHECK(ores_logical_frames_v1_begin(a, 7, 1, &ta));
    CHECK(ores_logical_frames_v1_push(a, ta, &f1));
    CHECK(!ores_logical_frames_v1_read(a, f0, 0, &out, sizeof out));
    CHECK(ores_logical_frames_v1_read(a, f1, 0, &out, sizeof out) && out == 0);
    CHECK(!ores_logical_frames_v1_close(a)); /* active turn */
    CHECK(ores_logical_frames_v1_pop(a, f1));
    CHECK(ores_logical_frames_v1_end(a, ta));
    CHECK(ores_logical_frames_v1_begin(b, 8, 1, &tb));
    CHECK(ores_logical_frames_v1_push(b, tb, &fb));
    CHECK(ores_logical_frames_v1_pop(b, fb));
    CHECK(ores_logical_frames_v1_end(b, tb));
    /* Two physical carriers can hold independent actor turns concurrently. */
    Worker w1 = {a, 0}, w2 = {a, 0};
    pthread_t p1, p2;
    CHECK(pthread_create(&p1, NULL, concurrent, &w1) == 0);
    CHECK(pthread_create(&p2, NULL, concurrent, &w2) == 0);
    CHECK(pthread_join(p1, NULL) == 0);
    CHECK(pthread_join(p2, NULL) == 0);
    CHECK(w1.success && w2.success);
    CHECK(ores_logical_frames_v1_close(a));
    CHECK(ores_logical_frames_v1_close(b));
    CHECK(ores_logical_frames_v1_close(duplicate));
    puts("logical frames: actor quota, LIFO, zeroization, stale handles, concurrent lanes PASS");
    return 0;
}
