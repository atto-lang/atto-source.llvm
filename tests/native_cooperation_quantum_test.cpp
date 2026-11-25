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
        require(checkDue == (i % 128 == 0));
        if (checkDue) require(q.shouldCooperateAtCheck() == (i == 512));
    }
    require(FakeClock::reads.load() == 5);
    require(q.completedBackedgesThisDispatch() == 512);
}

static void overdue_no_yield_region_checks_every_new_boundary() {
    reset_clock();
    Quantum q;
    q.beginDispatch();
    for (int i = 1; i <= 511; ++i)
        require(q.completedBackedgeNeedsCheck() == (i % 128 == 0));
    require(q.completedBackedgeNeedsCheck());
    require(q.shouldCooperateAtCheck());
    // After a protected section runs past the fairness cap, the next
    // opportunity remains periodic (512 + 128), never every backedge.
    for (int i = 513; i <= 640; ++i) {
        const bool due = q.completedBackedgeNeedsCheck();
        require(due == (i == 640));
        if (due) require(q.shouldCooperateAtCheck());
    }
    require(q.completedBackedgesThisDispatch() == 640);
    require(FakeClock::reads.load() == 2);
}

static void timer_slow_path() {
    reset_clock();
    Quantum q;
    q.beginDispatch();
    for (int i = 0; i < 127; ++i) require(!q.completedBackedgeNeedsCheck());
    FakeClock::nanos.store(2'050'000);
    require(FakeClock::reads.load() == 2);
    require(q.completedBackedgeNeedsCheck());
    require(q.shouldCooperateAtCheck());
    require(FakeClock::reads.load() == 3);
}

static void independent_watchdog_requests_next_safe_boundary() {
    reset_clock();
    Quantum q;
    q.beginDispatch();
    for (int i = 0; i < 127; ++i)
        require(!q.completedBackedgeNeedsCheck());
    require(q.completedBackedgeNeedsCheck());
    require(FakeClock::reads.load() == 2);
    require(q.shouldCooperateAtCheck(true));
    require(FakeClock::reads.load() == 2); // watcher request needs no clock read
    require(!q.shouldCooperateAtCheck(false));
    require(FakeClock::reads.load() == 2); // consumed ticket cannot poll twice
}

static void await_is_not_cooperate() {
    reset_clock();
    Quantum q;
    q.beginDispatch();
    q.recordedCooperate();
    FakeClock::nanos.store(30'000'000'000LL);
    q.beginDispatch();
    require(q.timeSinceLastCooperate() == std::chrono::seconds(30));
    for (int i = 0; i < 128; ++i) q.completedBackedgeNeedsCheck();
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
static void short_resumptions_cannot_evade_poll_phase() {
    reset_clock();
    Quantum q;
    std::uint64_t cumulative = 0;
    int polls = 0;
    for (int turn = 0; turn < 8; ++turn) {
        q.beginDispatch();
        for (int i = 0; i < 127; ++i) {
            ++cumulative;
            const bool due = q.completedBackedgeNeedsCheck();
            require(due == (cumulative % 128 == 0));
            if (due) {
                ++polls;
                // First ticket is usable. A second invocation with no new
                // completed backedge cannot re-enter the expensive runtime.
                require(!q.shouldCooperateAtCheck());
                const int after = FakeClock::reads.load();
                require(!q.shouldCooperateAtCheck());
                require(FakeClock::reads.load() == after);
            } else {
                // Even a buggy caller cannot force an expensive per-step poll.
                const int before = FakeClock::reads.load();
                require(!q.shouldCooperateAtCheck());
                require(FakeClock::reads.load() == before);
            }
        }
        require(q.completedBackedgesThisDispatch() == 127);
    }
    require(polls == 7);
    require(FakeClock::reads.load() == 1 + 8 + 7);
}

static void million_fast_backedges_do_not_read_the_clock() {
    reset_clock();
    Quantum q;
    q.beginDispatch();
    for (std::uint32_t i = 1; i <= 1'048'576; ++i)
        require(q.completedBackedgeNeedsCheck() == (i % 128 == 0));
    require(FakeClock::reads.load() == 2);
    require(q.completedBackedgesThisDispatch() == 1'048'576);
}


// Clock with an exact signed 64-bit nanosecond representation: no duration
// casts can hide an overflow/regression in the test itself.
struct ExtremeClock {
    using rep = std::int64_t;
    using period = std::nano;
    using duration = std::chrono::duration<rep, period>;
    using time_point = std::chrono::time_point<ExtremeClock>;
    static constexpr bool is_steady = true;
    static std::atomic<rep> ticks;
    static std::atomic<int> reads;
    static time_point now() noexcept {
        reads.fetch_add(1, std::memory_order_relaxed);
        return time_point(duration(ticks.load(std::memory_order_relaxed)));
    }
};
std::atomic<ExtremeClock::rep> ExtremeClock::ticks{0};
std::atomic<int> ExtremeClock::reads{0};

using ExtremeQuantum = oreslang::runtime::BasicSourceCooperationQuantum<ExtremeClock>;

static void extreme_clocks_fail_closed_on_slow_path() {
    constexpr auto upper = std::numeric_limits<std::int64_t>::max();
    constexpr auto lower = std::numeric_limits<std::int64_t>::min();

    // Old direct signed subtraction wraps to +11 and incorrectly clears
    // an overdue request. A ticket still permits at most one cold probe.
    ExtremeClock::ticks.store(upper - 5);
    ExtremeClock::reads.store(0);
    ExtremeQuantum wrapped;
    wrapped.beginDispatch();
    ExtremeClock::ticks.store(lower + 5);
    for (int i = 0; i < 127; ++i) require(!wrapped.completedBackedgeNeedsCheck());
    require(wrapped.completedBackedgeNeedsCheck());
    require(wrapped.shouldCooperateAtCheck());
    require(!wrapped.shouldCooperateAtCheck());
    require(ExtremeClock::reads.load() == 3);
    require(wrapped.timeSinceLastCooperate() == std::chrono::nanoseconds::max());

    // A backwards monotonic clock must not mint more scheduling time.
    ExtremeClock::ticks.store(10'000);
    ExtremeClock::reads.store(0);
    ExtremeQuantum regressed;
    regressed.beginDispatch();
    ExtremeClock::ticks.store(9'999);
    for (int i = 0; i < 128; ++i) regressed.completedBackedgeNeedsCheck();
    require(regressed.shouldCooperateAtCheck());
    require(regressed.timeSinceLastCooperate() == std::chrono::nanoseconds::max());

    // Even a mathematically positive delta greater than INT64_MAX may
    // never flow through signed chrono subtraction or duration_cast.
    ExtremeClock::ticks.store(lower + 7);
    ExtremeClock::reads.store(0);
    ExtremeQuantum huge;
    huge.beginDispatch();
    ExtremeClock::ticks.store(upper - 7);
    for (int i = 0; i < 128; ++i) huge.completedBackedgeNeedsCheck();
    require(huge.shouldCooperateAtCheck());
    require(huge.timeSinceLastCooperate() == std::chrono::nanoseconds::max());
}


static void overdue_no_yield_debt_survives_physical_dispatch() {
    reset_clock();
    Quantum q;
    q.beginDispatch();
    for (int i = 1; i <= 512; ++i)
        require(q.completedBackedgeNeedsCheck() == (i % 128 == 0));
    // A guard-held actor cannot park, but an overdue request is observable
    // after the independently compiler-verified no-yield scope exits.
    require(q.pendingHandoffOnNoYieldExit());
    q.beginDispatch();
    require(q.pendingHandoffOnNoYieldExit());
    for (int i = 1; i <= 128; ++i)
        require(q.completedBackedgeNeedsCheck() == (i == 128));
    require(q.shouldCooperateAtCheck());
    require(FakeClock::reads.load() == 3);
    q.recordedCooperate();
    require(!q.pendingHandoffOnNoYieldExit());
    q.beginDispatch();
    require(!q.pendingHandoffOnNoYieldExit());
    for (int i = 0; i < 128; ++i) q.completedBackedgeNeedsCheck();
    require(!q.shouldCooperateAtCheck());
}

static void unserved_watchdog_request_survives_await() {
    reset_clock();
    Quantum q;
    q.beginDispatch();
    for (int i = 0; i < 128; ++i) q.completedBackedgeNeedsCheck();
    require(q.shouldCooperateAtCheck(true));
    require(q.pendingHandoffOnNoYieldExit());
    q.beginDispatch(); // plain await: NOT a cooperation acknowledgment
    for (int i = 0; i < 128; ++i) q.completedBackedgeNeedsCheck();
    const int before = FakeClock::reads.load();
    require(q.shouldCooperateAtCheck(false));
    require(FakeClock::reads.load() == before);
    q.recordedCooperate();
    require(!q.pendingHandoffOnNoYieldExit());
}

int main() {
    reduction_fast_path();
    overdue_no_yield_region_checks_every_new_boundary();
    timer_slow_path();
    independent_watchdog_requests_next_safe_boundary();
    await_is_not_cooperate();
    repeated_dispatches_keep_task_context();
    short_resumptions_cannot_evade_poll_phase();
    million_fast_backedges_do_not_read_the_clock();
    extreme_clocks_fail_closed_on_slow_path();
    overdue_no_yield_debt_survives_physical_dispatch();
    unserved_watchdog_request_survives_await();
    std::cout << "native quantum 128/512/2ms: PASS (host-only, no guest await parity)\n";
}
