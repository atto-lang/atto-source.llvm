#include "oreslang/runtime/NativeActorScheduler.h"
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <pthread.h>
#include <thread>

extern "C" int oreslang_rt_spin_init(std::int64_t*) noexcept;
extern "C" int oreslang_rt_spin_resume(std::int64_t*, std::int64_t) noexcept;
extern "C" int oreslang_rt_spin_cancel(std::int64_t*) noexcept;

[[noreturn]] static void die(const char* reason) {
    std::fprintf(stderr, "ONE_CARRIER_AOT_FAIL: %s\n", reason);
    std::abort();
}
static void check(bool condition, const char* reason) {
    if (!condition) die(reason);
}
struct TestState {
    std::int64_t frame[2]{};
    pthread_t carrier{};
    bool carrier_assigned = false;  // only one physical scheduler worker writes
    std::atomic<bool> setup_complete{false};
    std::atomic<unsigned> executing{0}, actor_a{0}, actor_b{0},
                          errors{0}, canceled{0}, denied{0};
    std::atomic<int> first_b_a{-1};
};

static void verify_carrier(TestState& state) {
    if (state.executing.fetch_add(1) != 0) state.errors.fetch_add(1);
    if (!state.carrier_assigned) {
        state.carrier = pthread_self();
        state.carrier_assigned = true;
    } else if (!pthread_equal(state.carrier, pthread_self())) {
        state.errors.fetch_add(1);
    }
}
static void release_carrier(TestState& state) {
    if (state.executing.fetch_sub(1) != 1) state.errors.fetch_add(1);
}

static void actor_turn(void* ptr, size_t actor, uint64_t payload) {
    auto& state = *static_cast<TestState*>(ptr);
    if (actor == 0 && payload == 0) {
        // Testing-only producer admission barrier: both actor mailboxes must
        // be populated before the first genuine guest CPU dispatch.
        while (!state.setup_complete.load(std::memory_order_acquire))
            std::this_thread::yield();
    }
    verify_carrier(state);
    if (actor == 0) {
        if (payload == 64) {
            if (oreslang_rt_spin_resume(state.frame, 64) != -1)
                state.errors.fetch_add(1);
            state.denied.fetch_add(1);
        } else {
            const auto before = state.actor_a.fetch_add(1);
            if (payload != before || oreslang_rt_spin_resume(state.frame, 64) != 1)
                state.errors.fetch_add(1);
        }
    } else if (actor == 1) {
        const auto before = state.actor_b.fetch_add(1);
        if (payload != before) state.errors.fetch_add(1);
        int expected = -1;
        state.first_b_a.compare_exchange_strong(
            expected, static_cast<int>(state.actor_a.load()));
    } else {
        state.errors.fetch_add(1);
    }
    release_carrier(state);
}

static void control_turn(void* ptr, size_t actor, uint64_t payload) {
    auto& state = *static_cast<TestState*>(ptr);
    verify_carrier(state);
    if (actor != 0 || payload != 987 ||
            oreslang_rt_spin_cancel(state.frame) != 1) {
        state.errors.fetch_add(1);
    }
    state.canceled.fetch_add(1);
    release_carrier(state);
}

int main() {
    TestState state;
    check(oreslang_rt_spin_init(state.frame) == 1, "frame init");
    auto* scheduler = ores_native_actor_scheduler_open_control(
        2, 128, 1, actor_turn, control_turn, &state);
    check(scheduler != nullptr, "scheduler open");
    const auto a = ores_native_actor_scheduler_spawn(scheduler, 0);
    const auto b = ores_native_actor_scheduler_spawn(scheduler, 1);
    check(a.scheduler_domain != 0 && b.scheduler_domain == a.scheduler_domain,
          "actor spawn");
    check(ores_native_actor_scheduler_try_send(scheduler, a, 0) ==
          ORES_NATIVE_SEND_ACCEPTED, "first A mail");
    for (unsigned i = 1; i < 64; ++i)
        check(ores_native_actor_scheduler_try_send(scheduler, a, i) ==
              ORES_NATIVE_SEND_ACCEPTED, "queued A mail");
    for (unsigned i = 0; i < 64; ++i)
        check(ores_native_actor_scheduler_try_send(scheduler, b, i) ==
              ORES_NATIVE_SEND_ACCEPTED, "queued B mail");
    state.setup_complete.store(true, std::memory_order_release);
    check(ores_native_actor_scheduler_drain(scheduler), "drain turns");
    check(ores_native_actor_scheduler_started_carriers(scheduler) == 1,
          "expected exactly one physical carrier");
    check(state.errors.load() == 0 && state.executing.load() == 0,
          "wrong actor identity, overlapping turn or mailbox disorder");
    check(state.actor_a.load() == 64 && state.actor_b.load() == 64,
          "fairness starvation or lost mail");
    check(state.first_b_a.load() >= 0 && state.first_b_a.load() < 64,
          "B did not progress until A finished its infinite guest");
    check(state.frame[0] == 1 && state.frame[1] == 41 + 64 * 64,
          "compiled frame replay or missed updates");

    // Cancellation executes on A's reserved CONTROL lane, not on the
    // supervising host thread. Subsequent already-scheduled guest resumes
    // must not resurrect the frame or run any user loop effect.
    check(ores_native_actor_scheduler_publish_control(scheduler, a, 987),
          "cancel CONTROL enqueue");
    check(ores_native_actor_scheduler_drain(scheduler), "cancel CONTROL drain");
    check(state.canceled.load() == 1 && state.frame[0] == 2 &&
          state.frame[1] == 0, "cancel did not retire and zeroize frame");
    check(ores_native_actor_scheduler_try_send(scheduler, a, 64) ==
          ORES_NATIVE_SEND_ACCEPTED, "post-cancel test enqueue");
    check(ores_native_actor_scheduler_drain(scheduler), "post-cancel drain");
    check(state.denied.load() == 1 && state.actor_a.load() == 64 &&
          state.frame[0] == 2 && state.frame[1] == 0,
          "post-cancel guest callback was re-admitted");

    check(ores_native_actor_scheduler_stop(scheduler, a), "A stop");
    check(ores_native_actor_scheduler_stop(scheduler, b), "B stop");
    check(ores_native_actor_scheduler_poll_done(scheduler, a), "A not done");
    check(ores_native_actor_scheduler_poll_done(scheduler, b), "B not done");
    ores_native_actor_scheduler_retire(scheduler);
    check(ores_native_actor_scheduler_try_destroy(scheduler), "scheduler destroy");
    std::puts("AOT_NATIVE_ONE_CARRIER_TWO_ACTOR_FAIRNESS_PASS");
    return 0;
}
