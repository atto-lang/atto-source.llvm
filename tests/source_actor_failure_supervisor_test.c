/* Exercise real SourceActorBootstrap + NativeOresVM root-supervisor bridge.
 * Synthetic native receiver callbacks use the same statically linked ABI
 * emitted by LLVM. This is not a generated .atto module or guest AOT proof. */
#define _POSIX_C_SOURCE 200809L
#include "oreslang/runtime/SourceActorBootstrap.h"
#include <stdatomic.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define CHECK(c) do { if (!(c)) { \
    fprintf(stderr, "SOURCE_ACTOR_FAILURE_BRIDGE_FAIL line=%d expr=%s\n", __LINE__, #c); \
    abort(); } } while (0)

extern int oreslang_source_actor_frame_push(void);
extern int oreslang_source_actor_frame_store_i64(int64_t value);
extern int oreslang_source_actor_frame_load_i64(int64_t* out);
extern int oreslang_source_actor_frame_pop(void);

static atomic_int a_first, a_failed, b_finished;

static void initialize(OresActorI64Heap* heap, uint64_t domain, uint64_t epoch) {
    (void)heap; (void)domain; (void)epoch;
}
static int64_t receiver_a(OresActorI64Heap* heap, uint64_t domain,
                          uint64_t epoch, int64_t message) {
    (void)heap; (void)domain; (void)epoch;
    CHECK(oreslang_source_actor_frame_push());
    if (message == 7) {
        /* A second push violates the one-root-frame contract; it must
         * poison only actor A, not return a fake successful zero message. */
        CHECK(!oreslang_source_actor_frame_push());
        atomic_store_explicit(&a_failed, 1, memory_order_release);
        return 0;
    }
    CHECK(message == 9);
    CHECK(oreslang_source_actor_frame_store_i64(message));
    int64_t restored = 0;
    CHECK(oreslang_source_actor_frame_load_i64(&restored));
    CHECK(restored == 9);
    CHECK(oreslang_source_actor_frame_pop());
    atomic_store_explicit(&a_first, 1, memory_order_release);
    return restored;
}
static int64_t receiver_b(OresActorI64Heap* heap, uint64_t domain,
                          uint64_t epoch, int64_t message) {
    (void)heap; (void)domain; (void)epoch;
    CHECK(message == 42);
    CHECK(oreslang_source_actor_frame_push());
    CHECK(oreslang_source_actor_frame_store_i64(message));
    int64_t restored = 0;
    CHECK(oreslang_source_actor_frame_load_i64(&restored));
    CHECK(restored == 42);
    CHECK(oreslang_source_actor_frame_pop());
    atomic_store_explicit(&b_finished, 1, memory_order_release);
    return restored;
}
static void wait_bool(const atomic_int* value) {
    for (int retry = 0; retry < 4000; ++retry) {
        if (atomic_load_explicit(value, memory_order_acquire) != 0) return;
        struct timespec t = {0, 1000000};
        nanosleep(&t, NULL);
    }
    CHECK(0);
}
int main(void) {
    atomic_init(&a_first, 0);
    atomic_init(&a_failed, 0);
    atomic_init(&b_finished, 0);
    void* a = oreslang_source_actor_spawn(initialize, receiver_a, 1);
    void* b = oreslang_source_actor_spawn(initialize, receiver_b, 1);
    CHECK(a && b);
    CHECK(oreslang_source_actor_try_send(a, 9));
    CHECK(oreslang_source_actor_try_send(a, 7));
    CHECK(oreslang_source_actor_try_send(b, 42));
    wait_bool(&a_first);
    wait_bool(&a_failed);
    wait_bool(&b_finished);
    OresSourceActorMessageReceipt first = {0}, sibling = {0};
    for (int i = 0; i < 4000; ++i) {
        CHECK(oreslang_source_actor_message_receipt(a, &first));
        CHECK(oreslang_source_actor_message_receipt(b, &sibling));
        if (first.processed_callbacks == 2 && sibling.processed_callbacks == 1
            && first.stop_requested) break;
        struct timespec t = {0, 1000000};
        nanosleep(&t, NULL);
    }
    CHECK(first.processed_callbacks == 2);
    CHECK(first.successful_messages == 1 && first.failed_messages == 1);
    CHECK(first.last_success_result == 9);
    CHECK(first.failure_code == ORES_SOURCE_ACTOR_FAILURE_FRAME);
    CHECK(first.stop_requested);
    CHECK(sibling.processed_callbacks == 1);
    CHECK(sibling.successful_messages == 1 && sibling.failed_messages == 0);
    CHECK(sibling.last_success_result == 42 && sibling.failure_code == 0);
    CHECK(!oreslang_source_actor_try_send(a, 999));

    /* A self-sealed due to frame failure. This trusted-host JOIN must not
     * have to request a second STOP to obtain physical DONE and finalization. */
    oreslang_source_actor_join_done(a);
    /* B is healthy; normal stop is still distinct from a failure. */
    oreslang_source_actor_stop(b);
    CHECK(oreslang_source_actor_test_terminal_notices() == 2);
    CHECK(oreslang_source_actor_test_panic_notices() == 1);
    CHECK(oreslang_source_actor_test_stopped_notices() == 1);
    CHECK(oreslang_source_actor_test_last_panic_code() == ORES_SOURCE_ACTOR_FAILURE_FRAME);
    puts("SOURCE_ACTOR_FAILURE_SUPERVISOR_PASS A=failed_1_success_1_last_9 B=42 notices=2 finalized=2");
    return 0;
}
