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
    NESTED_ACTOR_STATE,
    CAS_DUPLICATE_PATCH,
    CAS_MISSING_VERSION,
    CAS_VERSION_SKIP,
    CAS_IMMUTABLE_FIELD,
    CAS_FORGED_CARRIER,
    CAS_VERSION_OVERFLOW
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
    } else if (kind >= CAS_DUPLICATE_PATCH) {
        ores_actor_packed_state_leave(state, turn);
        OresActorPackedState* ml = ores_actor_packed_state_open(91, 5, 3);
        OresActorPackedStateTurn t = ores_actor_packed_state_enter(ml, 91, 5);
        ores_actor_packed_state_field_init_mutable(ml, t.domain, t.epoch, 0,
                kind == CAS_VERSION_OVERFLOW ? INT64_MAX : 1);
        ores_actor_packed_state_field_init_mutable(
                ml, t.domain, t.epoch, 1, 5);
        ores_actor_packed_state_field_init(ml, t.domain, t.epoch, 2, 99);
        const uint64_t revision =
            ores_actor_packed_state_mutation_revision(ml, t.domain, t.epoch);
        OresActorScalarPatch full[] = {{0, 2}, {1, 4}};
        if (kind == CAS_DUPLICATE_PATCH) {
            const OresActorScalarPatch duplicate[] = {{0, 2}, {0, 2}};
            (void) ores_actor_packed_state_compare_publish(
                ml, t.domain, t.epoch, 0, 1, revision, duplicate, 2);
        } else if (kind == CAS_MISSING_VERSION) {
            const OresActorScalarPatch missing[] = {{1, 4}};
            (void) ores_actor_packed_state_compare_publish(
                ml, t.domain, t.epoch, 0, 1, revision, missing, 1);
        } else if (kind == CAS_VERSION_SKIP) {
            const OresActorScalarPatch skipped[] = {{0, 3}};
            (void) ores_actor_packed_state_compare_publish(
                ml, t.domain, t.epoch, 0, 1, revision, skipped, 1);
        } else if (kind == CAS_IMMUTABLE_FIELD) {
            const OresActorScalarPatch immutable[] = {{0, 2}, {2, 7}};
            (void) ores_actor_packed_state_compare_publish(
                ml, t.domain, t.epoch, 0, 1, revision, immutable, 2);
        } else if (kind == CAS_FORGED_CARRIER) {
            std::thread intruder([&] {
                (void) ores_actor_packed_state_compare_publish(
                    ml, t.domain, t.epoch, 0, 1, revision, full, 2);
            });
            intruder.join();
        } else if (kind == CAS_VERSION_OVERFLOW) {
            (void) ores_actor_packed_state_compare_publish(
                ml, t.domain, t.epoch, 0, INT64_MAX, revision, full, 2);
        }
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

    // Actual ActorPackedState + ActorArena runtime, not a state simulator:
    // every optimizer publication replaces the complete scalar image.
    OresActorPackedState* ml = ores_actor_packed_state_open(90, 7, 3);
    auto step = ores_actor_packed_state_enter(ml, 90, 7);
    ores_actor_packed_state_field_init_mutable(
        ml, step.domain, step.epoch, 0, 1); // weight version
    ores_actor_packed_state_field_init_mutable(
        ml, step.domain, step.epoch, 1, 5); // scalar weight
    ores_actor_packed_state_field_init_mutable(
        ml, step.domain, step.epoch, 2, 0); // optimizer iteration
    const uint64_t initialRevision =
        ores_actor_packed_state_mutation_revision(ml, step.domain, step.epoch);
    CHECK(initialRevision == 3);
    const OresActorScalarPatch firstUpdate[] = {{0, 2}, {1, 4}, {2, 1}};
    CHECK(ores_actor_packed_state_compare_publish(
        ml, step.domain, step.epoch, 0, 1, initialRevision, firstUpdate, 3) == 1);
    CHECK(ores_actor_packed_state_field_read(
        ml, step.domain, step.epoch, 0) == 2);
    CHECK(ores_actor_packed_state_field_read(
        ml, step.domain, step.epoch, 1) == 4);
    CHECK(ores_actor_packed_state_field_read(
        ml, step.domain, step.epoch, 2) == 1);
    // Exact duplicate CAS is a miss with ZERO mutations.
    CHECK(ores_actor_packed_state_compare_publish(
        ml, step.domain, step.epoch, 0, 1, initialRevision, firstUpdate, 3) == 0);
    CHECK(ores_actor_packed_state_field_read(
        ml, step.domain, step.epoch, 2) == 1);
    ores_actor_packed_state_leave(ml, step);

    // Three physical carriers, sequential ownership migration. This does not
    // claim concurrently executing turns for the same actor.
    int mlFailed = 0;
    for (int round = 0; round < 3; ++round) {
        std::thread carrierTurn([&, round] {
            auto t = ores_actor_packed_state_enter(ml, 90, 7);
            const int64_t expected = 2 + round;
            const uint64_t revision =
                ores_actor_packed_state_mutation_revision(
                    ml, t.domain, t.epoch);
            const OresActorScalarPatch updates[] = {
                {0, expected + 1}, {1, 4 - round}, {2, expected}
            };
            if (ores_actor_packed_state_compare_publish(
                    ml, t.domain, t.epoch, 0, expected, revision, updates, 3) != 1
                || ores_actor_packed_state_field_read(
                    ml, t.domain, t.epoch, 0) != expected + 1
                || ores_actor_packed_state_field_read(
                    ml, t.domain, t.epoch, 2) != expected)
                mlFailed = 1;
            ores_actor_packed_state_leave(ml, t);
        });
        carrierTurn.join();
    }
    CHECK(!mlFailed);
    step = ores_actor_packed_state_enter(ml, 90, 7);
    CHECK(ores_actor_packed_state_field_read(
        ml, step.domain, step.epoch, 0) == 5);
    CHECK(ores_actor_packed_state_field_read(
        ml, step.domain, step.epoch, 2) == 4);
    // Staging AFTER an intervening ordinary field write must not authorize
    // an optimizer candidate derived from a previous snapshot revision.
    const uint64_t staleSnapshotRevision =
        ores_actor_packed_state_mutation_revision(
            ml, step.domain, step.epoch);
    ores_actor_packed_state_field_write(
        ml, step.domain, step.epoch, 1, 99);
    const OresActorScalarPatch staleCandidate[] = {
        {0, 6}, {1, 1}, {2, 5}
    };
    CHECK(ores_actor_packed_state_compare_publish(
        ml, step.domain, step.epoch, 0, 5, staleSnapshotRevision,
        staleCandidate, 3) == 0);
    CHECK(ores_actor_packed_state_field_read(
        ml, step.domain, step.epoch, 1) == 99);
    CHECK(ores_actor_packed_state_field_read(
        ml, step.domain, step.epoch, 0) == 5);
    ores_actor_packed_state_retire(ml);
    CHECK(ores_actor_packed_state_try_destroy(ml) == 0);
    ores_actor_packed_state_leave(ml, step);
    CHECK(ores_actor_packed_state_try_destroy(ml) == 1);

#if defined(__unix__) || defined(__APPLE__)
    for (int i = DUPLICATE_INIT; i <= CAS_VERSION_OVERFLOW; ++i)
        CHECK(rejects((enum Misuse) i));
#endif
    puts("PACKED_SOURCE_ACTOR_STATE_OK");
    return 0;
}
