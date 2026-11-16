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
    std::uint64_t polls = 0;
    for (int i = 1; i <= 4096; ++i) {
        const bool checkDue = q.completedBackedgeNeedsCheck();
        require(checkDue == (i % 128 == 0));
        if (checkDue) {
            ++polls;
            require(q.shouldCooperateAtCheck() == (i >= 512));
        }
    }
    require(polls == 32);
    require(FakeClock::reads.load() == 5); // only first 3 polls read time
    require(q.completedBackedgesThisDispatch() == 4096);
}

static void overdue_no_yield_region_checks_only_at_next_boundary() {
    reset_clock();
    Quantum q;
    q.beginDispatch();
    for (int i = 1; i < 512; ++i)
        require(q.completedBackedgeNeedsCheck() == (i % 128 == 0));
    require(q.completedBackedgeNeedsCheck());
    require(q.shouldCooperateAtCheck());
    // An overdue no-yield scope still must not invoke the slow scheduler
    // path for each new iteration; first subsequent poll is at 640.
    for (int i = 513; i < 640; ++i)
        require(!q.completedBackedgeNeedsCheck());
    require(q.completedBackedgeNeedsCheck());
    require(q.shouldCooperateAtCheck());
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
    require(FakeClock::reads.load() == 2);
    require(!q.shouldCooperateAtCheck(false));
    require(FakeClock::reads.load() == 2); // no second clock read for same ticket
}

static void direct_slow_path_calls_cannot_skip_sampling_policy() {
    reset_clock();
    Quantum q;
    q.beginDispatch();
    FakeClock::nanos.store(200'000'000); // overdue in wall-clock time
    std::uint64_t observed = 0;
    // Adversarial eager caller invokes the expensive API EVERY iteration.
    // The API itself must enforce periodicity, including watchdog requests
    // and already exhausted reduction budgets.
    for (int i = 1; i <= 100000; ++i) {
        const bool due = q.completedBackedgeNeedsCheck();
        require(due == (i % 128 == 0));
        const bool permitted = q.shouldCooperateAtCheck(true);
        require(permitted == due);
        if (permitted) ++observed;
        require(!q.shouldCooperateAtCheck(true)); // one-shot ticket
    }
    require(observed == 100000 / 128);
    require(q.completedBackedgesThisDispatch() == 100000);
    require(FakeClock::reads.load() == 2); // watchdog fast return, no clock
    q.beginDispatch();
    require(!q.shouldCooperateAtCheck(true)); // reset invalidates stale ticket
}
static void time_poll_without_due_ticket_never_samples_clock() {
    reset_clock();
    Quantum q;
    q.beginDispatch();
    const int baseline = FakeClock::reads.load();
    for (int i = 1; i < 128; ++i) {
        require(!q.completedBackedgeNeedsCheck());
        require(!q.shouldCooperateAtCheck());
    }
    require(FakeClock::reads.load() == baseline);
    require(q.completedBackedgeNeedsCheck());
    require(!q.shouldCooperateAtCheck());
    require(FakeClock::reads.load() == baseline + 1);
    require(!q.shouldCooperateAtCheck());
    require(FakeClock::reads.load() == baseline + 1);
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
    // After a host clock anomaly, the untrusted fairness timer must not
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
    overdue_no_yield_region_checks_only_at_next_boundary();
    timer_slow_path();
    independent_watchdog_requests_next_safe_boundary();
    direct_slow_path_calls_cannot_skip_sampling_policy();
    time_poll_without_due_ticket_never_samples_clock();
    await_is_not_cooperate();
    repeated_dispatches_keep_task_context();
    std::cout << "native quantum 128/512/2ms: PASS (host-only, no guest await parity)\n";
}
