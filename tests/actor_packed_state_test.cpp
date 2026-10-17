#include "oreslang/runtime/ActorPackedState.h"
#include <stdint.h>
#include <stdio.h>
#include <thread>
#if defined(__unix__) || defined(__APPLE__)
#include <signal.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#define CHECK(x) do { if (!(x)) { \
    fprintf(stderr, "packed actor state failed:%d\n", __LINE__); return 1; \
} } while (0)

enum Misuse {
    DUPLICATE_INIT,
    WRITE_IMMUTABLE,
    READ_UNINITIALIZED,
    OUT_OF_RANGE,
    FOREIGN_OWNER,
    FOREIGN_CARRIER_TURN,
    NESTED_ACTOR_STATE
};

struct ForeignCarrier {
    OresActorPackedState* state;
    OresActorPackedStateTurn turn;
};

static void misuse(enum Misuse kind) {
    OresActorPackedState* state = ores_actor_packed_state_open(81, 3, 2);
    OresActorPackedStateTurn turn =
        ores_actor_packed_state_enter(state, 81, 3);
    ores_actor_packed_state_field_init(
        state, turn.domain, turn.epoch, 0, 10);

    if (kind == DUPLICATE_INIT) {
        ores_actor_packed_state_field_init(
            state, turn.domain, turn.epoch, 0, 11);
    } else if (kind == WRITE_IMMUTABLE) {
        ores_actor_packed_state_field_write(
            state, turn.domain, turn.epoch, 0, 11);
    } else if (kind == READ_UNINITIALIZED) {
        (void) ores_actor_packed_state_field_read(
            state, turn.domain, turn.epoch, 1);
    } else if (kind == OUT_OF_RANGE) {
        (void) ores_actor_packed_state_field_read(
            state, turn.domain, turn.epoch, 2);
    } else if (kind == FOREIGN_OWNER) {
        ores_actor_packed_state_leave(state, turn);
        (void) ores_actor_packed_state_enter(state, 82, 3);
    } else if (kind == FOREIGN_CARRIER_TURN) {
        ForeignCarrier stolen{state, turn};
        std::thread foreign([&] {
            (void) ores_actor_packed_state_field_read(
                stolen.state, stolen.turn.domain, stolen.turn.epoch, 0);
        });
        foreign.join();
    } else if (kind == NESTED_ACTOR_STATE) {
        OresActorPackedState* other = ores_actor_packed_state_open(82, 3, 1);
        (void) ores_actor_packed_state_enter(other, 82, 3);
    }
}

#if defined(__unix__) || defined(__APPLE__)
static int rejects(enum Misuse kind) {
    pid_t child = fork();
    if (child < 0) return 0;
    if (child == 0) {
        struct rlimit limit = {0, 0};
        (void) setrlimit(RLIMIT_CORE, &limit);
        misuse(kind);
        _exit(91);
    }
    int status = 0;
    return waitpid(child, &status, 0) == child
        && WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT;
}
#endif

int main(void) {
    OresActorPackedState* state = ores_actor_packed_state_open(81, 3, 2);
    OresActorPackedStateTurn first =
        ores_actor_packed_state_enter(state, 81, 3);
    ores_actor_packed_state_field_init(
        state, first.domain, first.epoch, 0, 10);
    ores_actor_packed_state_field_init_mutable(
        state, first.domain, first.epoch, 1, 20);
    CHECK(ores_actor_packed_state_field_read(
        state, first.domain, first.epoch, 0) == 10);
    CHECK(ores_actor_packed_state_field_read(
        state, first.domain, first.epoch, 1) == 20);
    ores_actor_packed_state_leave(state, first);

    int carrier_failed = 0;
    std::thread carrier([&] {
        OresActorPackedStateTurn turn =
            ores_actor_packed_state_enter(state, 81, 3);
        if (ores_actor_packed_state_field_read(
                state, turn.domain, turn.epoch, 0) != 10)
            carrier_failed = 1;
        if (ores_actor_packed_state_field_read(
                state, turn.domain, turn.epoch, 1) != 20)
            carrier_failed = 1;
        ores_actor_packed_state_field_write(
            state, turn.domain, turn.epoch, 1, 42);
        ores_actor_packed_state_leave(state, turn);
    });
    carrier.join();
    CHECK(!carrier_failed);

    OresActorPackedStateTurn last =
        ores_actor_packed_state_enter(state, 81, 3);
    CHECK(ores_actor_packed_state_field_read(
        state, last.domain, last.epoch, 0) == 10);
    CHECK(ores_actor_packed_state_field_read(
        state, last.domain, last.epoch, 1) == 42);
    ores_actor_packed_state_retire(state);
    CHECK(ores_actor_packed_state_try_destroy(state) == 0);
    ores_actor_packed_state_leave(state, last);
    CHECK(ores_actor_packed_state_try_destroy(state) == 1);

#if defined(__unix__) || defined(__APPLE__)
    for (int i = DUPLICATE_INIT; i <= NESTED_ACTOR_STATE; ++i)
        CHECK(rejects((enum Misuse) i));
#endif
    puts("PACKED_SOURCE_ACTOR_STATE_OK");
    return 0;
}
