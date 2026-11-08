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
    using rep = std::int64_t;
    using period = std::nano;
    using duration = std::chrono::nanoseconds;
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
static_assert(oreslang::runtime::detail::saturatingBackedgeBatch(
                  std::numeric_limits<std::uint64_t>::max() - 127, 128)
                  == std::numeric_limits<std::uint64_t>::max(),
              "cold batch addition must saturate instead of wrapping");
static_assert(oreslang::runtime::detail::saturatingBackedgeBatch(
                  std::numeric_limits<std::uint64_t>::max(), 128)
                  == std::numeric_limits<std::uint64_t>::max(),
              "saturation must be sticky without disabling subsequent polls");
static_assert(oreslang::runtime::detail::saturatingBackedgeBatch(512, 127) == 639,
              "the incomplete reduction window must count exactly");

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
        require(q.completedBackedgesThisDispatch() == static_cast<std::uint64_t>(i));
        if (checkDue) require(q.shouldCooperateAtCheck() == (i == 512));
    }
    require(FakeClock::reads.load() == 5);
    require(q.completedBackedgesThisDispatch() == 512);
}

static void overdue_no_yield_region_preserves_poll_cadence() {
    reset_clock();
    Quantum q;
    q.beginDispatch();
    for (int i = 1; i <= 511; ++i)
        require(q.completedBackedgeNeedsCheck() == (i % 128 == 0));
    require(q.completedBackedgeNeedsCheck());
    require(q.shouldCooperateAtCheck());
    // A no-yield region must NOT force a slow poll on every overdue loop.
    // Its exit can explicitly check the resumption boundary once.
    require(q.shouldCooperateAtCheck());
    for (int i = 513; i < 640; ++i)
        require(!q.completedBackedgeNeedsCheck());
    require(q.completedBackedgeNeedsCheck());
    require(q.shouldCooperateAtCheck());
    require(q.completedBackedgesThisDispatch() == 640);
    require(FakeClock::reads.load() == 2);
}

static void long_overdue_quantum_never_polls_every_iteration() {
    reset_clock();
    Quantum q;
    q.beginDispatch();
    int checks = 0;
    for (int i = 1; i <= 1'000'000; ++i) {
        const bool due = q.completedBackedgeNeedsCheck();
        require(due == (i % 128 == 0));
        if (due) {
            ++checks;
            require(q.shouldCooperateAtCheck() == (i >= 512));
        }
    }
    require(checks == 7'812);
    require(q.completedBackedgesThisDispatch() == 1'000'000);
    // The cold timer is sampled only in the first three sub-budget windows.
    require(FakeClock::reads.load() == 5);
    q.beginDispatch();
    require(q.completedBackedgesThisDispatch() == 0);
    for (int i = 1; i < 128; ++i) {
        require(!q.completedBackedgeNeedsCheck());
        require(q.completedBackedgesThisDispatch() == static_cast<std::uint64_t>(i));
    }
    require(q.completedBackedgeNeedsCheck());
    require(q.completedBackedgesThisDispatch() == 128);
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
    require(FakeClock::reads.load() == 3);
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
static void clock_regression_and_positive_wrap_fail_closed() {
    reset_clock();
    // After the host clock anomaly, the untrusted fairness timer must not
    // mistake a wrap for eleven nanoseconds of fresh execution.
    FakeClock::nanos.store(std::numeric_limits<std::int64_t>::max() - 5);
    Quantum q;
    q.beginDispatch();
    FakeClock::nanos.store(std::numeric_limits<std::int64_t>::min() + 5);
    for (int n = 1; n < 128; ++n)
        require(!q.completedBackedgeNeedsCheck());
    require(q.completedBackedgeNeedsCheck());
    require(q.shouldCooperateAtCheck());
    require(q.timeSinceLastCooperate() == std::chrono::nanoseconds::max());
    require(FakeClock::reads.load() == 4);

    // A backwards clock jump, even without arithmetic overflow, also
    // requests a safe soft cooperation rather than extending the quantum.
    reset_clock();
    FakeClock::nanos.store(10'000);
    Quantum backwards;
    backwards.beginDispatch();
    FakeClock::nanos.store(9'999);
    for (int n = 1; n < 128; ++n)
        require(!backwards.completedBackedgeNeedsCheck());
    require(backwards.completedBackedgeNeedsCheck());
    require(backwards.shouldCooperateAtCheck());
    require(backwards.timeSinceLastCooperate() == std::chrono::nanoseconds::max());
}

static void enormous_positive_elapsed_time_saturates_without_signed_ub() {
    reset_clock();
    FakeClock::nanos.store(std::numeric_limits<std::int64_t>::min() + 10);
    Quantum q;
    q.beginDispatch();
    FakeClock::nanos.store(std::numeric_limits<std::int64_t>::max() - 10);
    for (int n = 1; n < 128; ++n)
        require(!q.completedBackedgeNeedsCheck());
    require(q.completedBackedgeNeedsCheck());
    require(q.shouldCooperateAtCheck());
    require(q.timeSinceLastCooperate() == std::chrono::nanoseconds::max());

    // The unsigned representation can express this enormous valid
    // mathematical delta even though the signed chrono duration cannot.
    auto now = FakeClock::time_point(
        FakeClock::duration(std::numeric_limits<std::int64_t>::max() - 10));
    auto started = FakeClock::time_point(
        FakeClock::duration(std::numeric_limits<std::int64_t>::min() + 10));
    require(oreslang::runtime::detail::elapsedClockTicksOrMax<FakeClock>(
        now, started) == std::numeric_limits<std::uint64_t>::max() - 20);
}

int main() {
    clock_regression_and_positive_wrap_fail_closed();
    enormous_positive_elapsed_time_saturates_without_signed_ub();
    reduction_fast_path();
    overdue_no_yield_region_preserves_poll_cadence();
    long_overdue_quantum_never_polls_every_iteration();
    timer_slow_path();
    independent_watchdog_requests_next_safe_boundary();
    await_is_not_cooperate();
    repeated_dispatches_keep_task_context();
    std::cout << "native quantum 128/512/2ms: PASS (host-only, no guest await parity)\n";
}
