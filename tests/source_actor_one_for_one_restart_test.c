/* Real native source-bootstrap + registered NativeOresVM one-for-one policy.
 * A permits one successful frame admission per actor incarnation. A's
 * second DATA fails under that unchanged quota, root supervisor authorizes
 * exactly one replacement after DONE/heap+frame cleanup, A2 handles 9,
 * and sibling B handles 42. Stale old ActorRef rejects further sends.
 * This is native C ABI proof; compiled .atto guest is a separate CI gate. */
#define _POSIX_C_SOURCE 200809L
#include "oreslang/runtime/SourceActorBootstrap.h"
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { \
  fprintf(stderr, "SOURCE_ACTOR_RESTART_FAIL line=%d condition=%s\n", __LINE__, #x); \
  abort(); } } while (0)
extern int oreslang_source_actor_frame_push(void);
extern int oreslang_source_actor_frame_store_i64(int64_t);
extern int oreslang_source_actor_frame_load_i64(int64_t*);
extern int oreslang_source_actor_frame_pop(void);

static atomic_int a_seven, a_nine, b_forty_two;
static void initialize(OresActorI64Heap* h, uint64_t d, uint64_t e) {
    (void)h; (void)d; (void)e;
}
static int64_t actor_a(OresActorI64Heap* h, uint64_t d, uint64_t e, int64_t msg) {
    (void)h; (void)d; (void)e;
    if (!oreslang_source_actor_frame_push()) return 0;
    CHECK(msg == 7 || msg == 9);
    CHECK(oreslang_source_actor_frame_store_i64(msg));
    int64_t value = 0;
    CHECK(oreslang_source_actor_frame_load_i64(&value));
    CHECK(oreslang_source_actor_frame_pop());
    CHECK(value == msg);
    if (msg == 7) atomic_fetch_add(&a_seven, 1);
    if (msg == 9) atomic_fetch_add(&a_nine, 1);
    return value;
}
static int64_t actor_b(OresActorI64Heap* h, uint64_t d, uint64_t e, int64_t msg) {
    (void)h; (void)d; (void)e;
    CHECK(msg == 42);
    CHECK(oreslang_source_actor_frame_push());
    CHECK(oreslang_source_actor_frame_store_i64(msg));
    int64_t value = 0;
    CHECK(oreslang_source_actor_frame_load_i64(&value));
    CHECK(oreslang_source_actor_frame_pop());
    CHECK(value == 42);
    atomic_fetch_add(&b_forty_two, 1);
    return value;
}
int main(int argc, char** argv) {
    const int exhaust_restart = argc == 2 && strcmp(argv[1], "--exhaust") == 0;
    CHECK(argc == 1 || exhaust_restart);
    atomic_init(&a_seven, 0); atomic_init(&a_nine, 0);
    atomic_init(&b_forty_two, 0);
    void* a = oreslang_source_actor_spawn_supervised_budgeted(
        initialize, actor_a, 1, 16, 1, 1);
    void* b = oreslang_source_actor_spawn(initialize, actor_b, 1);
    CHECK(a && b && a != b);
    CHECK(oreslang_source_actor_try_send(a, 7));
    CHECK(oreslang_source_actor_flush_blocking(a));
    CHECK(oreslang_source_actor_try_send(a, 8));
    CHECK(oreslang_source_actor_flush_blocking(a));
    OresSourceActorMessageReceipt old = {0};
    CHECK(oreslang_source_actor_message_receipt(a, &old));
    CHECK(old.processed_callbacks == 2);
    CHECK(old.successful_messages == 1 && old.failed_messages == 1);
    CHECK(old.last_success_result == 7);
    CHECK(old.failure_code == ORES_SOURCE_ACTOR_FAILURE_FRAME);
    CHECK(old.stop_requested);
    void* replacement = oreslang_source_actor_recover_blocking(a);
    CHECK(replacement && replacement != a && replacement != b);
    /* Old handle is a bounded tombstone with no heap, mailbox, or frames. */
    CHECK(!oreslang_source_actor_try_send(a, 123));
    CHECK(!oreslang_source_actor_recover_blocking(a));
    CHECK(oreslang_source_actor_message_receipt(a, &old));
    CHECK(old.last_success_result == 7);
    CHECK(oreslang_source_actor_try_send(replacement, 9));
    CHECK(oreslang_source_actor_flush_blocking(replacement));
    OresSourceActorMessageReceipt fresh = {0};
    CHECK(oreslang_source_actor_message_receipt(replacement, &fresh));
    CHECK(fresh.processed_callbacks == 1 && fresh.successful_messages == 1);
    CHECK(fresh.failed_messages == 0 && fresh.last_success_result == 9);
    CHECK(fresh.failure_code == 0);
    CHECK(oreslang_source_actor_try_send(b, 42));
    CHECK(oreslang_source_actor_flush_blocking(b));
    CHECK(atomic_load(&a_seven) == 1);
    CHECK(atomic_load(&a_nine) == 1);
    CHECK(atomic_load(&b_forty_two) == 1);
    if (exhaust_restart) {
        /* A2 received a fresh per-incarnation quota of one frame and
         * processed 9. Its next turn must fail *without* creating A3:
         * the supervisor's total one_for_one restart budget was spent. */
        CHECK(oreslang_source_actor_try_send(replacement, 10));
        CHECK(oreslang_source_actor_flush_blocking(replacement));
        CHECK(oreslang_source_actor_message_receipt(replacement, &fresh));
        CHECK(fresh.processed_callbacks == 2);
        CHECK(fresh.successful_messages == 1 && fresh.failed_messages == 1);
        CHECK(fresh.last_success_result == 9);
        CHECK(fresh.failure_code == ORES_SOURCE_ACTOR_FAILURE_FRAME);
        CHECK(fresh.stop_requested);
        CHECK(!oreslang_source_actor_recover_blocking(replacement));
    }
    oreslang_source_actor_stop(replacement);
    oreslang_source_actor_stop(b);
    CHECK(oreslang_source_actor_test_terminal_notices() == 3);
    CHECK(oreslang_source_actor_test_panic_notices() == (exhaust_restart ? 2u : 1u));
    CHECK(oreslang_source_actor_test_stopped_notices() == (exhaust_restart ? 1u : 2u));
    CHECK(oreslang_source_actor_test_last_panic_code()
          == ORES_SOURCE_ACTOR_FAILURE_FRAME);
    if (exhaust_restart)
        puts("SOURCE_ACTOR_RESTART_BUDGET_EXHAUSTED_PASS A=PANIC A2=PANIC B=42 A3=NONE");
    else
        puts("SOURCE_ACTOR_ONE_FOR_ONE_PASS A=1_failed_1 A2=9 B=42 PANIC=1 STOPPED=2");
    return 0;
}
