#include "oreslang/runtime/ActorRegionV1.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(__unix__) || defined(__APPLE__)
#include <signal.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#define CHECK(x) do { if (!(x)) { \
    fprintf(stderr, "ActorRegionV1 host contract failed at %d: %s\n", __LINE__, #x); \
    return 1; \
} } while (0)

static int basic_recycling_and_loan(void) {
    CHECK(sizeof(OresActorOffset32) == 4);
    OresActorRegionV1* a = ores_actor_region_v1_open(77, 1, 2, 64, 64);
    OresActorRegionV1* b = ores_actor_region_v1_open(78, 1, 2, 64, 64);
    CHECK(ores_actor_region_v1_prefault(a));
    OresArenaTurn at = ores_actor_region_v1_enter(a, 77, 1);
    OresActorRegionV1Ref first = ores_actor_region_v1_alloc(a, at, 16);
    CHECK(first.offset.storage == 1);
    OresActorRegionV1Ref neighbor = ores_actor_region_v1_alloc(a, at, 8);
    CHECK(neighbor.offset.storage == 65);
    const uint64_t payload = UINT64_C(0x0102030405060708);
    ores_actor_region_v1_write(a, at, first, 8, &payload, sizeof(payload));
    uint64_t readback = 0;
    ores_actor_region_v1_read(a, at, first, 8, &readback, sizeof(readback));
    CHECK(readback == payload);
    OresActorRegionV1Loan loan = ores_actor_region_v1_borrow(a, at, first);
    ores_actor_region_v1_leave(a, at);
    ores_actor_region_v1_retire(a);
    CHECK(!ores_actor_region_v1_try_destroy(a));
    ores_actor_region_v1_release(a, loan);
    CHECK(ores_actor_region_v1_try_destroy(a));

    OresArenaTurn bt = ores_actor_region_v1_enter(b, 78, 1);
    OresActorRegionV1Ref other = ores_actor_region_v1_alloc(b, bt, 16);
    CHECK(other.offset.storage == 1);
    CHECK(other.proof.domain != first.proof.domain);
    uint64_t empty = UINT64_MAX;
    ores_actor_region_v1_read(b, bt, other, 0, &empty, sizeof(empty));
    CHECK(empty == 0);
    ores_actor_region_v1_drop(b, bt, &other);
    CHECK(!other.offset.storage);
    OresActorRegionV1Ref replacement = ores_actor_region_v1_alloc(b, bt, 16);
    CHECK(replacement.offset.storage == 1);
    CHECK(replacement.proof.generation > 1);
    ores_actor_region_v1_read(b, bt, replacement, 0, &empty, sizeof(empty));
    CHECK(empty == 0); /* dropped extent physically scrubbed */
    ores_actor_region_v1_drop(b, bt, &replacement);
    ores_actor_region_v1_leave(b, bt);
    ores_actor_region_v1_retire(b);
    CHECK(ores_actor_region_v1_try_destroy(b));
    return 0;
}
static int repeated_slot_recycling(void) {
    OresActorRegionV1* a = ores_actor_region_v1_open(80, 9, 1, 32, 32);
    OresArenaTurn turn = ores_actor_region_v1_enter(a, 80, 9);
    uint64_t generation = 0;
    for (unsigned i = 0; i < 4096; ++i) {
        OresActorRegionV1Ref h = ores_actor_region_v1_alloc(a, turn, 8);
        CHECK(h.offset.storage == 1 && h.proof.generation > generation);
        generation = h.proof.generation;
        uint64_t value = i + 10;
        ores_actor_region_v1_write(a, turn, h, 0, &value, sizeof(value));
        uint64_t result = 0;
        ores_actor_region_v1_read(a, turn, h, 0, &result, sizeof(result));
        CHECK(result == value);
        ores_actor_region_v1_drop(a, turn, &h);
        CHECK(h.offset.storage == 0);
    }
    OresActorRegionV1Ref kept = ores_actor_region_v1_alloc(a, turn, 8);
    ores_actor_region_v1_leave(a, turn);
    OresArenaTurn resumed = ores_actor_region_v1_enter(a, 80, 9);
    uint64_t empty = UINT64_MAX;
    ores_actor_region_v1_read(a, resumed, kept, 0, &empty, sizeof(empty));
    CHECK(empty == 0);
    ores_actor_region_v1_drop(a, resumed, &kept);
    ores_actor_region_v1_leave(a, resumed);
    ores_actor_region_v1_retire(a);
    CHECK(ores_actor_region_v1_try_destroy(a));
    return 0;
}

#if defined(__unix__) || defined(__APPLE__)
enum Misuse {
    FORGED_OFFSET, FORGED_GENERATION, FORGED_DOMAIN, CROSS_ACTOR,
    FIELD_OVERRUN, DROP_WITH_LOAN, WRITE_WITH_LOAN, STALE_SLOT_REUSE,
    DOUBLE_DROP, USE_AFTER_TURN, WRONG_OWNER, OVERSIZED_REGION,
    LOAN_DOUBLE_RELEASE, LOAN_SIDECAR_TAMPER, LOAN_OFFSET_TAMPER
};
static void attempt(enum Misuse what) {
    if (what == OVERSIZED_REGION) {
        (void)ores_actor_region_v1_open(1, 1, 4096, 1024 * 1024, 1024);
        return;
    }
    OresActorRegionV1* a = ores_actor_region_v1_open(100, 1, 1, 32, 32);
    if (what == WRONG_OWNER) {
        (void)ores_actor_region_v1_enter(a, 101, 1);
        return;
    }
    OresArenaTurn turn = ores_actor_region_v1_enter(a, 100, 1);
    OresActorRegionV1Ref h = ores_actor_region_v1_alloc(a, turn, 8);
    uint64_t value = 22;
    if (what == FORGED_OFFSET) {
        h.offset.storage = 2;
        ores_actor_region_v1_read(a, turn, h, 0, &value, sizeof(value));
    } else if (what == FORGED_GENERATION) {
        ++h.proof.generation;
        ores_actor_region_v1_read(a, turn, h, 0, &value, sizeof(value));
    } else if (what == FORGED_DOMAIN) {
        ++h.proof.domain;
        ores_actor_region_v1_read(a, turn, h, 0, &value, sizeof(value));
    } else if (what == FIELD_OVERRUN) {
        ores_actor_region_v1_read(a, turn, h, 1, &value, sizeof(value));
    } else if (what == DROP_WITH_LOAN || what == WRITE_WITH_LOAN) {
        (void)ores_actor_region_v1_borrow(a, turn, h);
        if (what == DROP_WITH_LOAN) ores_actor_region_v1_drop(a, turn, &h);
        else ores_actor_region_v1_write(a, turn, h, 0, &value, sizeof(value));
    } else if (what == STALE_SLOT_REUSE || what == DOUBLE_DROP) {
        OresActorRegionV1Ref copy = h;
        ores_actor_region_v1_drop(a, turn, &h);
        if (what == STALE_SLOT_REUSE) {
            OresActorRegionV1Ref fresh = ores_actor_region_v1_alloc(a, turn, 8);
            if (fresh.offset.storage != copy.offset.storage) _exit(92);
        }
        ores_actor_region_v1_drop(a, turn, &copy);
    } else if (what == USE_AFTER_TURN) {
        ores_actor_region_v1_leave(a, turn);
        ores_actor_region_v1_read(a, turn, h, 0, &value, sizeof(value));
    } else if (what == CROSS_ACTOR) {
        ores_actor_region_v1_leave(a, turn);
        OresActorRegionV1* other = ores_actor_region_v1_open(101, 1, 1, 32, 32);
        OresArenaTurn ot = ores_actor_region_v1_enter(other, 101, 1);
        ores_actor_region_v1_read(other, ot, h, 0, &value, sizeof(value));
    } else if (what == LOAN_DOUBLE_RELEASE) {
        OresActorRegionV1Loan loan = ores_actor_region_v1_borrow(a, turn, h);
        ores_actor_region_v1_release(a, loan);
        ores_actor_region_v1_release(a, loan);
    } else if (what == LOAN_SIDECAR_TAMPER || what == LOAN_OFFSET_TAMPER) {
        OresActorRegionV1Loan loan = ores_actor_region_v1_borrow(a, turn, h);
        if (what == LOAN_SIDECAR_TAMPER) ++loan.proof.generation;
        else ++loan.ref.offset.storage;
        ores_actor_region_v1_release(a, loan);
    }
}
static int must_abort(enum Misuse what) {
    const pid_t pid = fork();
    if (pid < 0) return 0;
    if (pid == 0) {
        struct rlimit limit = {0, 0};
        (void)setrlimit(RLIMIT_CORE, &limit);
        attempt(what);
        _exit(93);
    }
    int status = 0;
    return waitpid(pid, &status, 0) == pid && WIFSIGNALED(status) &&
           WTERMSIG(status) == SIGABRT;
}
#endif

int main(void) {
    CHECK(basic_recycling_and_loan() == 0);
    CHECK(repeated_slot_recycling() == 0);
#if defined(__unix__) || defined(__APPLE__)
    for (int scenario = FORGED_OFFSET; scenario <= LOAN_OFFSET_TAMPER; ++scenario)
        CHECK(must_abort((enum Misuse)scenario));
#endif
    puts("ACTOR_REGION_V1_HOST_ONLY|offset_bits=32|generational=1|scrub=1|result=PASS");
    return 0;
}
