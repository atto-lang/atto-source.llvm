#include "oreslang/runtime/SourceCooperationQuantum.h"
#include <atomic>
#include <stdexcept>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <limits>
#include <type_traits>
#include <utility>

struct FakeClock {
    using rep = std::chrono::steady_clock::rep;
    using period = std::chrono::steady_clock::period;
    using duration = std::chrono::steady_clock::duration;
    using time_point = std::chrono::time_point<FakeClock>;
    static constexpr bool is_steady = true;
    static std::atomic<std::int64_t> nanos;
    static std::atomic<int> reads;
    static time_point now() noexcept {
        reads.fetch_add(1, std::memory_order_relaxed);
        return time_point(std::chrono::duration_cast<duration>(
            std::chrono::nanoseconds(nanos.load(std::memory_order_relaxed))));
    }
};
std::atomic<std::int64_t> FakeClock::nanos{0};
std::atomic<int> FakeClock::reads{0};

using Quantum = oreslang::runtime::BasicSourceCooperationQuantum<FakeClock>;
// A quantum must never be value-copied when a SourceTask resumes on a new
// carrier: that would reset or duplicate its fairness and watchdog history.
static_assert(!std::is_copy_constructible_v<Quantum>, "quantum must be task-owned");
static_assert(!std::is_copy_assignable_v<Quantum>, "quantum must not be reassigned");
static_assert(!std::is_move_constructible_v<Quantum>, "continuation must retain one quantum");
static_assert(!std::is_move_assignable_v<Quantum>, "quantum must not migrate separately");
static_assert(std::is_same_v<decltype(std::declval<Quantum>().completedBackedgesThisDispatch()), std::uint64_t>,
              "per-dispatch reductions must not wrap at 32 bits");
static_assert(oreslang::runtime::detail::saturatingBackedgeSuccessor(
                  std::numeric_limits<std::uint64_t>::max() - 1)
                  == std::numeric_limits<std::uint64_t>::max(),
              "the last representable reduction must be counted");
static_assert(oreslang::runtime::detail::saturatingBackedgeSuccessor(
                  std::numeric_limits<std::uint64_t>::max())
                  == std::numeric_limits<std::uint64_t>::max(),
              "a wrapped reduction counter could bypass the budget");

static void require(bool ok) {
    if (!ok) throw std::runtime_error("native cooperation-quantum invariant failed");
}

static void reset_clock() {
    FakeClock::nanos.store(0);
    FakeClock::reads.store(0);
}

static void reduction_fast_path() {
    reset_clock();
    Quantum q;
    q.beginDispatch();
    require(FakeClock::reads.load() == 2);
    for (int i = 1; i <= 512; ++i) {
        const bool checkDue = q.completedBackedgeNeedsCheck();
        require(checkDue == (i % 64 == 0));
        if (checkDue) require(q.shouldCooperateAtCheck() == (i == 512));
    }
    require(FakeClock::reads.load() == 9);
    require(q.completedBackedgesThisDispatch() == 512);
}

static void overdue_no_yield_region_latches_handoff_without_per_edge_polls() {
    reset_clock();
    Quantum q;
    q.beginDispatch();
    for (int i = 1; i <= 512; ++i) {
        const bool due = q.completedBackedgeNeedsCheck();
        require(due == (i % 64 == 0));
        if (due) require(q.shouldCooperateAtCheck() == (i == 512));
    }
    // The critical section was still held when the 512-cap was reached.
    // Later backedges must NOT invoke the slow poll on every iteration.
    require(q.pendingHandoffOnNoYieldExit());
    for (int i = 513; i <= 1025; ++i) {
        const bool due = q.completedBackedgeNeedsCheck();
        require(due == (i % 64 == 0));
        if (due) require(q.shouldCooperateAtCheck());
        require(q.pendingHandoffOnNoYieldExit());
    }
    require(q.completedBackedgesThisDispatch() == 1025);
    require(FakeClock::reads.load() == 9); // 2 start + only seven early slow polls
    // The caller must invoke pendingHandoffOnNoYieldExit() when the guard is
    // finally dropped, even if the exit is between two batch boundaries.
    q.recordedCooperate();
    q.beginDispatch();
    require(!q.pendingHandoffOnNoYieldExit());
}


static void configurable_soft_poll_interval_preserves_hard_cap() {
    for (std::uint32_t interval : {128u, 256u, 512u}) {
        reset_clock();
        Quantum q(interval);
        q.beginDispatch();
        require(FakeClock::reads.load() == 2);
        for (std::uint32_t n = 1; n <= 512; ++n) {
            const bool due = q.completedBackedgeNeedsCheck();
            require(due == (n % interval == 0));
            if (due) require(q.shouldCooperateAtCheck() == (n == 512));
        }
        // At 512 the hard cap is clock-free, regardless of chosen batch.
        require(FakeClock::reads.load() == 2 + 512 / static_cast<int>(interval) - 1);
        // A sticky request is actionable when the protected scope exits.
        // Normal fast-path backedges still poll only at their own cadence.
        for (int n = 513; n < 530; ++n) {
            const bool due = q.completedBackedgeNeedsCheck();
            require(due == (n % interval == 0));
            if (due) require(q.shouldCooperateAtCheck());
            require(q.pendingHandoffOnNoYieldExit());
        }
        require(FakeClock::reads.load() == 2 + 512 / static_cast<int>(interval) - 1);
    }
    bool rejected = false;
    try { Quantum bad(100); } catch (const std::invalid_argument&) { rejected = true; }
    require(rejected);
}

static void timer_slow_path() {
    reset_clock();
    Quantum q;
    q.beginDispatch();
    for (int i = 0; i < 63; ++i) require(!q.completedBackedgeNeedsCheck());
    FakeClock::nanos.store(2'050'000);
    require(FakeClock::reads.load() == 2);
    require(q.completedBackedgeNeedsCheck());
    require(q.shouldCooperateAtCheck());
    require(q.pendingHandoffOnNoYieldExit()); // critical exit immediately observes timed debt
    require(FakeClock::reads.load() == 3);
}

static void independent_watchdog_requests_next_safe_boundary() {
    reset_clock();
    Quantum q;
    q.beginDispatch();
    for (int i = 0; i < 63; ++i)
        require(!q.completedBackedgeNeedsCheck());
    require(q.completedBackedgeNeedsCheck());
    require(FakeClock::reads.load() == 2);
    require(q.shouldCooperateAtCheck(true));
    require(q.pendingHandoffOnNoYieldExit());
    require(FakeClock::reads.load() == 2); // watchdog request needs no clock read
    require(q.shouldCooperateAtCheck(false)); // request stays sticky until handoff
    require(FakeClock::reads.load() == 2);
    q.recordedCooperate();
    require(!q.pendingHandoffOnNoYieldExit());
}

static void overdue_debt_survives_unrelated_await_and_dispatch_reset() {
    for (std::uint32_t interval : {64u, 128u, 256u, 512u}) {
        reset_clock();
        Quantum q(interval);
        q.beginDispatch();
        for (std::uint32_t i = 1; i <= 512; ++i) {
            const bool due = q.completedBackedgeNeedsCheck();
            require(due == (i % interval == 0));
            if (due) (void)q.shouldCooperateAtCheck();
        }
        require(q.pendingHandoffOnNoYieldExit());
        const int readsAtCap = FakeClock::reads.load();
        // A task might be suspended for a reason other than rt cooperate:
        // resetting a dispatch MUST NOT discard the old request.
        q.beginDispatch();
        require(q.completedBackedgesThisDispatch() == 0);
        require(q.pendingHandoffOnNoYieldExit());
        require(FakeClock::reads.load() == readsAtCap + 1);
        for (std::uint32_t i = 1; i <= interval; ++i) {
            require(q.completedBackedgeNeedsCheck() == (i == interval));
        }
        require(q.shouldCooperateAtCheck()); // sticky even without elapsed time
        require(FakeClock::reads.load() == readsAtCap + 1);
        q.recordedCooperate(); // only actual scheduler handoff acknowledges debt
        q.beginDispatch();
        require(!q.pendingHandoffOnNoYieldExit());
    }
}

static void watchdog_debt_survives_dispatch_reset() {
    reset_clock();
    Quantum q(128);
    q.beginDispatch();
    for (int i = 0; i < 128; ++i)
        (void)q.completedBackedgeNeedsCheck();
    require(q.shouldCooperateAtCheck(true));
    require(q.pendingHandoffOnNoYieldExit());
    q.beginDispatch();
    require(q.pendingHandoffOnNoYieldExit());
    require(q.completedBackedgesThisDispatch() == 0);
    q.recordedCooperate();
    q.beginDispatch();
    require(!q.pendingHandoffOnNoYieldExit());
}

static void over_cap_reset_latches_debt_without_expensive_poll() {
    reset_clock();
    Quantum q(512);
    q.beginDispatch();
    for (int i = 0; i < 513; ++i) {
        require(q.completedBackedgeNeedsCheck() == (i == 511));
        // The host deliberately omitted the one periodic slow poll while
        // holding a no-yield guard. beginDispatch cannot forgive that debt.
    }
    require(q.pendingHandoffOnNoYieldExit());
    const int before = FakeClock::reads.load();
    q.beginDispatch();
    require(FakeClock::reads.load() == before + 1);
    require(q.pendingHandoffOnNoYieldExit());
    q.recordedCooperate();
    q.beginDispatch();
    require(!q.pendingHandoffOnNoYieldExit());
}

static void await_is_not_cooperate() {
    reset_clock();
    Quantum q;
    q.beginDispatch();
    q.recordedCooperate();
    FakeClock::nanos.store(30'000'000'000LL);
    q.beginDispatch();
    require(q.timeSinceLastCooperate() == std::chrono::seconds(30));
    for (int i = 0; i < 64; ++i) q.completedBackedgeNeedsCheck();
    require(!q.shouldCooperateAtCheck());
    q.recordedCooperate();
    require(q.timeSinceLastCooperate() == std::chrono::nanoseconds::zero());
}

static void repeated_dispatches_keep_task_context() {
    reset_clock();
    Quantum q;
    for (int turn = 0; turn < 2048; ++turn) {
        FakeClock::nanos.fetch_add(2'000'000);
        q.beginDispatch();
        for (int i = 0; i < 512; ++i) {
            if (q.completedBackedgeNeedsCheck())
                require(q.shouldCooperateAtCheck() == (i == 511));
        }
        q.recordedCooperate();
    }
    require(q.completedBackedgesThisDispatch() == 512);
    require(q.timeSinceLastCooperate() == std::chrono::nanoseconds::zero());
}
int main() {
    reduction_fast_path();
    configurable_soft_poll_interval_preserves_hard_cap();
    overdue_no_yield_region_latches_handoff_without_per_edge_polls();
    timer_slow_path();
    independent_watchdog_requests_next_safe_boundary();
    overdue_debt_survives_unrelated_await_and_dispatch_reset();
    watchdog_debt_survives_dispatch_reset();
    over_cap_reset_latches_debt_without_expensive_poll();
    await_is_not_cooperate();
    repeated_dispatches_keep_task_context();
    std::cout << "native quantum 64/128/256/512 sticky pending: PASS (host-only, no guest await parity)\n";
}
