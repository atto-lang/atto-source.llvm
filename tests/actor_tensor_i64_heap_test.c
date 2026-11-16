#include "oreslang/runtime/ActorTensorI64.h"
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#if defined(__unix__) || defined(__APPLE__)
#include <signal.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#define CHECK(expr) do { if (!(expr)) { \
  fprintf(stderr, "actor tensor contract failed at %d: %s\n", __LINE__, #expr); \
  exit(1); \
} } while (0)

static OresActorI64Heap* a_heap;
static OresActorI64Heap* b_heap;
static OresActorTurn a_turn, b_turn;
static OresActorTensorI64* a_tensor;
static OresActorTensorI64* moved_tensor;

#if defined(__unix__) || defined(__APPLE__)
static void expect_abort(int scenario) {
    pid_t child = fork();
    CHECK(child >= 0);
    if (child == 0) {
        switch (scenario) {
            case 1:
                ores_actor_tensor_i64_write(a_tensor, a_heap, a_turn,
                    1, 2, 2, 0, 99); break; /* read loan freezes writer */
            case 2:
                ores_actor_tensor_i64_take(moved_tensor, a_tensor);
                break; /* read loan freezes take */
            case 3:
                ores_actor_tensor_i64_drop(a_tensor, a_heap, a_turn);
                break; /* read loan freezes drop */
            case 4:
                /* One admitted turn per carrier: suspend A, run B, then try
                 * to reach A's storage through B's heap authority. */
                ores_actor_i64_heap_leave(a_heap, a_turn);
                b_turn = ores_actor_i64_heap_enter(b_heap, 102, 1);
                (void)ores_actor_tensor_i64_read(a_tensor, b_heap, b_turn,
                    1, 2, 2, 0);
                break; /* foreign heap/actor authority */
            case 5:
                (void)ores_actor_tensor_i64_read(a_tensor, a_heap, a_turn,
                    1, 2, 2, 0);
                break; /* moved-from original */
            case 6:
                (void)ores_actor_tensor_i64_read(moved_tensor, a_heap, a_turn,
                    4, 2, 2, 1);
                break; /* out of bounds source view */
            case 7:
                (void)ores_actor_tensor_i64_read(moved_tensor, a_heap, a_turn,
                    1, 2, 2, 0);
                break; /* physically dropped descriptor */
            default: _exit(77);
        }
        _exit(0);
    }
    int status = 0;
    CHECK(waitpid(child, &status, 0) == child);
    CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT);
}
#else
static void expect_abort(int scenario) { (void)scenario; }
#endif

int main(void) {
#if defined(__unix__) || defined(__APPLE__)
    struct rlimit core = {0, 0};
    (void)setrlimit(RLIMIT_CORE, &core);
#endif
    CHECK(ores_actor_tensor_i64_valid_span(INT64_MAX, INT64_MAX - 1, 1, 1));
    CHECK(!ores_actor_tensor_i64_valid_span(INT64_MAX, 1, INT64_MAX, 2));
    CHECK(ores_actor_tensor_i64_valid_span(0, 0, 0, 1));
    CHECK(!ores_actor_tensor_i64_valid_span(8, 8, 1, 1));
    CHECK(!ores_actor_tensor_i64_valid_span(8, 9, 0, 1));
    CHECK(!ores_actor_tensor_i64_valid_span(8, 0, 2, 0));

    a_heap = ores_actor_i64_heap_open(101, 1, 8);
    b_heap = ores_actor_i64_heap_open(102, 1, 8);

    int64_t av[5] = {3, 5, 7, 11, 13};
    int64_t bv[2] = {17, 19};
    OresActorTensorI64 a = {0}, b = {0}, moved = {0};
    a_tensor = &a;
    moved_tensor = &moved;
    /* A native carrier runs at most one admitted actor turn at a time
     * (ActorOwnedI64Heap carrier TLS), so A and B turns alternate. */
    b_turn = ores_actor_i64_heap_enter(b_heap, 102, 1);
    ores_actor_tensor_i64_init(&b, b_heap, b_turn, bv, 2);
    CHECK(ores_actor_tensor_i64_read(&b, b_heap, b_turn, 0, 2, 1, 0) == 17);
    ores_actor_i64_heap_leave(b_heap, b_turn);

    a_turn = ores_actor_i64_heap_enter(a_heap, 101, 1);
    ores_actor_tensor_i64_init(&a, a_heap, a_turn, av, 5);
    CHECK(ores_actor_tensor_i64_read(&a, a_heap, a_turn, 1, 2, 2, 0) == 5);
    CHECK(ores_actor_tensor_i64_read(&a, a_heap, a_turn, 1, 2, 2, 1) == 11);

    OresActorTensorI64Loan loan = {0};
    ores_actor_tensor_i64_borrow_read(&a, a_heap, a_turn, &loan);
    expect_abort(1); /* cannot write while shared view held */
    expect_abort(2); /* cannot transfer while shared view held */
    expect_abort(3); /* cannot physically drop while shared view held */
    expect_abort(4); /* no actor B access to actor A storage */
    ores_actor_i64_heap_leave(a_heap, a_turn);

    /* Actor B continues independently while A's loan survives suspension. */
    b_turn = ores_actor_i64_heap_enter(b_heap, 102, 1);
    ores_actor_tensor_i64_write(&b, b_heap, b_turn, 0, 2, 1, 1, 20);
    CHECK(ores_actor_tensor_i64_read(&b, b_heap, b_turn, 0, 2, 1, 1) == 20);
    ores_actor_tensor_i64_write(&b, b_heap, b_turn, 0, 2, 1, 1, 19);
    ores_actor_i64_heap_leave(b_heap, b_turn);

    ores_actor_tensor_i64_release(&loan);
    a_turn = ores_actor_i64_heap_enter(a_heap, 101, 1);
    ores_actor_tensor_i64_take(&moved, &a);
    expect_abort(5); /* moved descriptor invalid even if slot tokens remain */
    ores_actor_tensor_i64_write(&moved, a_heap, a_turn, 1, 2, 2, 0, 23);
    ores_actor_tensor_i64_write(&moved, a_heap, a_turn, 1, 2, 2, 1, 29);
    CHECK(ores_actor_tensor_i64_read(&moved, a_heap, a_turn, 0, 5, 1, 0) == 3);
    CHECK(ores_actor_tensor_i64_read(&moved, a_heap, a_turn, 0, 5, 1, 2) == 7);
    CHECK(ores_actor_tensor_i64_read(&moved, a_heap, a_turn, 0, 5, 1, 4) == 13);
    CHECK(ores_actor_tensor_i64_read(&moved, a_heap, a_turn, 1, 2, 2, 0) == 23);
    CHECK(ores_actor_tensor_i64_read(&moved, a_heap, a_turn, 1, 2, 2, 1) == 29);
    expect_abort(6); /* strict bounds validation, even with live storage */
    ores_actor_tensor_i64_drop(&moved, a_heap, a_turn);
    expect_abort(7); /* stale descriptor rejected */
    ores_actor_i64_heap_leave(a_heap, a_turn);
    ores_actor_i64_heap_retire(a_heap);
    CHECK(ores_actor_i64_heap_try_destroy(a_heap));
    a_heap = NULL;

    b_turn = ores_actor_i64_heap_enter(b_heap, 102, 1);
    CHECK(ores_actor_tensor_i64_read(&b, b_heap, b_turn, 0, 2, 1, 0) == 17);
    CHECK(ores_actor_tensor_i64_read(&b, b_heap, b_turn, 0, 2, 1, 1) == 19);
    ores_actor_tensor_i64_drop(&b, b_heap, b_turn);
    ores_actor_i64_heap_leave(b_heap, b_turn);
    ores_actor_i64_heap_retire(b_heap);
    CHECK(ores_actor_i64_heap_try_destroy(b_heap));

    /* Physical actor retirement must remain blocked by a live tensor loan. */
    OresActorI64Heap* c_heap = ores_actor_i64_heap_open(103, 1, 4);
    OresActorTurn c_turn = ores_actor_i64_heap_enter(c_heap, 103, 1);
    OresActorTensorI64 c = {0};
    int64_t cv[2] = {40, 2};
    ores_actor_tensor_i64_init(&c, c_heap, c_turn, cv, 2);
    OresActorTensorI64Loan pin = {0};
    ores_actor_tensor_i64_borrow_read(&c, c_heap, c_turn, &pin);
    ores_actor_i64_heap_leave(c_heap, c_turn);
    ores_actor_i64_heap_retire(c_heap);
    CHECK(!ores_actor_i64_heap_try_destroy(c_heap));
    ores_actor_tensor_i64_release(&pin);
    CHECK(ores_actor_i64_heap_try_destroy(c_heap));

    puts("ACTOR-TENSOR-I64-HOST-ONLY-OK");
    return 0;
}
