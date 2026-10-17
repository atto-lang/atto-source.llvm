#include "oreslang/runtime/ActorOwnedI64Heap.h"
#include <stdint.h>
#include <stdio.h>
#if defined(__unix__) || defined(__APPLE__)
#include <signal.h>
#include <sys/resource.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "state write grant failure:%d\n", __LINE__); return 1; } } while (0)

enum Attack {
    BARE_WRITE, NO_GRANT, EXPIRED_GRANT, REPLAY_NEXT_TURN, IMMUTABLE_WRITE,
    READ_LOAN_UPGRADE, NESTED_GRANT, CROSS_HEAP_GRANT, WRONG_TURN,
    STOP_REVOKED_GRANT, AFTER_LEAVE
};

static void misuse(enum Attack kind) {
    OresActorI64Heap* heap = ores_actor_i64_heap_open(55, 3, 2);
    OresActorTurn turn = ores_actor_i64_heap_enter(heap, 55, 3);
    ores_actor_i64_heap_field_init_mutable(heap, turn.domain, turn.epoch, 0, 1);
    if (kind == BARE_WRITE) {
        ores_actor_i64_heap_field_write(heap, turn.domain, turn.epoch, 0, 42);
    } else if (kind == NO_GRANT) {
        ores_actor_i64_heap_field_write_granted(heap, turn.domain, turn.epoch, 0, 0, 42);
    } else if (kind == EXPIRED_GRANT) {
        uint64_t grant = ores_actor_i64_heap_state_write_begin(heap, turn.domain, turn.epoch);
        ores_actor_i64_heap_state_write_end(heap, turn.domain, turn.epoch, grant);
        ores_actor_i64_heap_field_write_granted(heap, turn.domain, turn.epoch, grant, 0, 42);
    } else if (kind == REPLAY_NEXT_TURN) {
        uint64_t grant = ores_actor_i64_heap_state_write_begin(heap, turn.domain, turn.epoch);
        ores_actor_i64_heap_leave(heap, turn);
        OresActorTurn current = ores_actor_i64_heap_enter(heap, 55, 3);
        ores_actor_i64_heap_field_write_granted(heap, current.domain, current.epoch, grant, 0, 42);
    } else if (kind == IMMUTABLE_WRITE) {
        ores_actor_i64_heap_field_init(heap, turn.domain, turn.epoch, 1, 7);
        uint64_t grant = ores_actor_i64_heap_state_write_begin(heap, turn.domain, turn.epoch);
        ores_actor_i64_heap_field_write_granted(heap, turn.domain, turn.epoch, grant, 1, 42);
    } else if (kind == READ_LOAN_UPGRADE) {
        OresActorSlot slot = {turn.domain, 0, 1};
        (void) ores_actor_i64_heap_borrow(heap, turn, slot);
        (void) ores_actor_i64_heap_state_write_begin(heap, turn.domain, turn.epoch);
    } else if (kind == NESTED_GRANT) {
        (void) ores_actor_i64_heap_state_write_begin(heap, turn.domain, turn.epoch);
        (void) ores_actor_i64_heap_state_write_begin(heap, turn.domain, turn.epoch);
    } else if (kind == CROSS_HEAP_GRANT) {
        OresActorI64Heap* other = ores_actor_i64_heap_open(56, 3, 2);
        OresActorTurn othert = ores_actor_i64_heap_enter(other, 56, 3);
        ores_actor_i64_heap_field_init_mutable(other, othert.domain, othert.epoch, 0, 7);
        uint64_t a = ores_actor_i64_heap_state_write_begin(heap, turn.domain, turn.epoch);
        uint64_t b = ores_actor_i64_heap_state_write_begin(other, othert.domain, othert.epoch);
        if (!a || !b || a == b) return;
        ores_actor_i64_heap_field_write_granted(other, othert.domain, othert.epoch, a, 0, 42);
    } else if (kind == WRONG_TURN) {
        uint64_t grant = ores_actor_i64_heap_state_write_begin(heap, turn.domain, turn.epoch);
        ores_actor_i64_heap_field_write_granted(heap, turn.domain, turn.epoch + 1, grant, 0, 42);
    } else if (kind == STOP_REVOKED_GRANT) {
        uint64_t grant = ores_actor_i64_heap_state_write_begin(heap, turn.domain, turn.epoch);
        ores_actor_i64_heap_retire(heap);
        ores_actor_i64_heap_field_write_granted(heap, turn.domain, turn.epoch, grant, 0, 42);
    } else if (kind == AFTER_LEAVE) {
        uint64_t grant = ores_actor_i64_heap_state_write_begin(heap, turn.domain, turn.epoch);
        ores_actor_i64_heap_leave(heap, turn);
        ores_actor_i64_heap_field_write_granted(heap, turn.domain, turn.epoch, grant, 0, 42);
    }
}
#if defined(__unix__) || defined(__APPLE__)
static int aborts(enum Attack kind) {
    pid_t pid = fork();
    if (pid < 0) return 0;
    if (pid == 0) {
        struct rlimit limit = {0, 0};
        (void) setrlimit(RLIMIT_CORE, &limit);
        misuse(kind);
        _exit(91);
    }
    int status = 0;
    return waitpid(pid, &status, 0) == pid &&
           WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT;
}
#endif

int main(void) {
    OresActorI64Heap* heap = ores_actor_i64_heap_open(55, 3, 2);
    OresActorTurn turn = ores_actor_i64_heap_enter(heap, 55, 3);
    ores_actor_i64_heap_field_init_mutable(heap, turn.domain, turn.epoch, 0, 1);
    uint64_t grant = ores_actor_i64_heap_state_write_begin(heap, turn.domain, turn.epoch);
    CHECK(grant != 0);
    ores_actor_i64_heap_field_write_granted(heap, turn.domain, turn.epoch, grant, 0, 42);
    CHECK(ores_actor_i64_heap_field_read(heap, turn.domain, turn.epoch, 0) == 42);
    ores_actor_i64_heap_state_write_end(heap, turn.domain, turn.epoch, grant);
    ores_actor_i64_heap_leave(heap, turn);
    OresActorTurn next = ores_actor_i64_heap_enter(heap, 55, 3);
    uint64_t next_grant = ores_actor_i64_heap_state_write_begin(heap, next.domain, next.epoch);
    CHECK(next_grant != 0 && next_grant != grant);
    ores_actor_i64_heap_field_write_granted(heap, next.domain, next.epoch, next_grant, 0, 43);
    ores_actor_i64_heap_state_write_end(heap, next.domain, next.epoch, next_grant);
    CHECK(ores_actor_i64_heap_field_read(heap, next.domain, next.epoch, 0) == 43);
    ores_actor_i64_heap_leave(heap, next);
    ores_actor_i64_heap_retire(heap);
    CHECK(ores_actor_i64_heap_try_destroy(heap) == 1);
#if defined(__unix__) || defined(__APPLE__)
    for (int kind = BARE_WRITE; kind <= AFTER_LEAVE; ++kind)
        CHECK(aborts((enum Attack) kind));
#endif
    return 0;
}
