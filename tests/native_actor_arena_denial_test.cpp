#include "oreslang/runtime/ActorArena.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <csignal>
#include <initializer_list>
#include <sys/resource.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define CHECK(expr) do { \
    if (!(expr)) { std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); std::abort(); } \
} while (false)

// Host-native release/abort regression. Aborted children have copy-on-write
// state and cannot corrupt the parent test process or claim guest execution.
enum class Case {
    wrong_owner,
    wrong_generation,
    foreign_handle,
    foreign_loan,
    stale_generation,
    double_loan_release,
    scratch_after_turn,
    retired_admission,
};

static void child(Case which) {
    OresActorArena* a = ores_actor_arena_open(101, 7, 2, 64, 128);
    OresActorArena* b = ores_actor_arena_open(202, 11, 2, 64, 128);
    CHECK(a != nullptr && b != nullptr);
    if (which == Case::wrong_owner) {
        (void)ores_actor_arena_enter(a, 202, 7);
    } else if (which == Case::wrong_generation) {
        (void)ores_actor_arena_enter(a, 101, 8);
    } else if (which == Case::foreign_handle) {
        const auto ta = ores_actor_arena_enter(a, 101, 7);
        const auto h = ores_actor_arena_alloc(a, ta, sizeof(std::uint64_t));
        ores_actor_arena_leave(a, ta);
        const auto tb = ores_actor_arena_enter(b, 202, 11);
        std::uint64_t result = 0;
        ores_actor_arena_read(b, tb, h, 0, &result, sizeof(result));
    } else if (which == Case::foreign_loan) {
        const auto ta = ores_actor_arena_enter(a, 101, 7);
        const auto h = ores_actor_arena_alloc(a, ta, sizeof(std::uint64_t));
        const auto loan = ores_actor_arena_borrow(a, ta, h);
        ores_actor_arena_leave(a, ta);
        ores_actor_arena_release(b, loan);
    } else if (which == Case::stale_generation) {
        const auto ta = ores_actor_arena_enter(a, 101, 7);
        const auto h = ores_actor_arena_alloc(a, ta, sizeof(std::uint64_t));
        ores_actor_arena_drop(a, ta, h);
        const auto replacement = ores_actor_arena_alloc(a, ta, sizeof(std::uint64_t));
        CHECK(replacement.index == h.index && replacement.generation != h.generation);
        std::uint64_t result = 0;
        ores_actor_arena_read(a, ta, h, 0, &result, sizeof(result));
    } else if (which == Case::double_loan_release) {
        const auto ta = ores_actor_arena_enter(a, 101, 7);
        const auto h = ores_actor_arena_alloc(a, ta, sizeof(std::uint64_t));
        const auto loan = ores_actor_arena_borrow(a, ta, h);
        ores_actor_arena_leave(a, ta);
        ores_actor_arena_release(a, loan);
        ores_actor_arena_release(a, loan);
    } else if (which == Case::scratch_after_turn) {
        const auto t1 = ores_actor_arena_enter(a, 101, 7);
        const auto scratch = ores_actor_arena_scratch_alloc(a, t1, sizeof(std::uint64_t), alignof(std::uint64_t));
        ores_actor_arena_leave(a, t1);
        const auto t2 = ores_actor_arena_enter(a, 101, 7);
        std::uint64_t result = 0;
        ores_actor_arena_scratch_read(a, t2, scratch, 0, &result, sizeof(result));
    } else if (which == Case::retired_admission) {
        ores_actor_arena_retire(a);
        (void)ores_actor_arena_enter(a, 101, 7);
    }
    // Any unauthorized operation returning normally is a failed negative test.
    _exit(23);
}

int main() {
    const struct rlimit no_core = {0, 0};
    CHECK(setrlimit(RLIMIT_CORE, &no_core) == 0);
    for (const Case scenario : {
        Case::wrong_owner,
        Case::wrong_generation,
        Case::foreign_handle,
        Case::foreign_loan,
        Case::stale_generation,
        Case::double_loan_release,
        Case::scratch_after_turn,
        Case::retired_admission,
    }) {
        const pid_t pid = fork();
        CHECK(pid >= 0);
        if (pid == 0) child(scenario);
        int status = 0;
        CHECK(waitpid(pid, &status, 0) == pid);
        CHECK(WIFSIGNALED(status));
        CHECK(WTERMSIG(status) == SIGABRT);
    }
    std::puts("native-actor-arena-denial: 8/8 abort gates passed");
    return 0;
}
