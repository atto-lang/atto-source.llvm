#include "oreslang/runtime/SourceCooperationQuantum.h"
#include <atomic>
#include <stdexcept>
#include <chrono>
#include <cstdint>
#include <iostream>
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
static_assert(std::is_same_v<decltype(std::declval<Quantum>().completedBackedgesThisDispatch()), std::uint64_t>,
              "per-dispatch reductions must not wrap at 32 bits");

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

static void timer_slow_path() {
    reset_clock();
    Quantum q;
    q.beginDispatch();
    for (int i = 0; i < 63; ++i) require(!q.completedBackedgeNeedsCheck());
    FakeClock::nanos.store(2'050'000);
    require(FakeClock::reads.load() == 2);
    require(q.completedBackedgeNeedsCheck());
    require(q.shouldCooperateAtCheck());
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
    timer_slow_path();
    await_is_not_cooperate();
    repeated_dispatches_keep_task_context();
    std::cout << "native quantum 64/512/2ms: PASS (host-only, no guest await parity)\n";
}
