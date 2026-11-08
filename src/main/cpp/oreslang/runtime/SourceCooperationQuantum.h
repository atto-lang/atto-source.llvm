#pragma once

#include <chrono>
#include <cstdint>
#include <limits>
#include <type_traits>

namespace oreslang::runtime {

// Saturation preserves the mandatory reduction cap even after an extremely
// prolonged no-yield critical section. Kept constexpr for boundary testing.
namespace detail {
constexpr std::uint64_t saturatingBackedgeSuccessor(
        std::uint64_t completed) noexcept {
    return completed == std::numeric_limits<std::uint64_t>::max()
        ? completed : completed + 1;
}

// Accumulate complete batches on the cold path; only the short countdown is
// touched on each committed reduction. Saturation cannot suppress later polls.
constexpr std::uint64_t saturatingBackedgeBatch(
        std::uint64_t completed, std::uint64_t batch) noexcept {
    return batch > std::numeric_limits<std::uint64_t>::max() - completed
        ? std::numeric_limits<std::uint64_t>::max() : completed + batch;
}

// Saturate elapsed time on clock regression or signed overflow. Subtracting
// chrono::time_point values directly invokes signed duration subtraction,
// which is undefined behavior when a fake/host clock crosses INT64 extremes.
// Unsigned subtraction is exact after checking that now >= started, even
// if the mathematical delta exceeds INT64_MAX.
template <typename Clock>
std::uint64_t elapsedClockTicksOrMax(
        typename Clock::time_point now,
        typename Clock::time_point started) noexcept {
    using rep = typename Clock::duration::rep;
    static_assert(std::is_integral_v<rep> && std::is_signed_v<rep> &&
                  sizeof(rep) <= sizeof(std::uint64_t),
                  "checkpoint clocks require bounded signed integral ticks");
    using unsigned_rep = std::make_unsigned_t<rep>;
    const rep current = now.time_since_epoch().count();
    const rep baseline = started.time_since_epoch().count();
    if (current < baseline) return std::numeric_limits<std::uint64_t>::max();
    return static_cast<std::uint64_t>(
        static_cast<unsigned_rep>(current) -
        static_cast<unsigned_rep>(baseline));
}
}  // namespace detail

/**
 * Host-only native scheduling quantum, not guest Oreslang await lowering.
 * Template clock permits deterministic testing without sleeps or time polling
 * inside a tight loop. The caller must own a compiler-proven, heap-resumable
 * post-commit continuation before acting on shouldCooperateAtCheck().
 */
template <typename Clock = std::chrono::steady_clock>
class BasicSourceCooperationQuantum final {
public:
    using time_point = typename Clock::time_point;
    static constexpr std::uint32_t kLoopCheckInterval = 128;
    static constexpr std::uint32_t kMaxBackedgesPerDispatch = 512;
    static constexpr auto kSoftDispatchQuantum = std::chrono::milliseconds(2);

    BasicSourceCooperationQuantum()
        : lastCooperate_(Clock::now()), dispatchStarted_(lastCooperate_) {}

    // A budget belongs to one logical task: copying or moving would split
    // its reduction history from the saved heap continuation. Carrier
    // migration transports the owning task, never a copied quantum.
    BasicSourceCooperationQuantum(const BasicSourceCooperationQuantum&) = delete;
    BasicSourceCooperationQuantum& operator=(const BasicSourceCooperationQuantum&) = delete;
    BasicSourceCooperationQuantum(BasicSourceCooperationQuantum&&) = delete;
    BasicSourceCooperationQuantum& operator=(BasicSourceCooperationQuantum&&) = delete;

    // Calling this after await restarts *only* the fairness quantum.
    void beginDispatch() noexcept {
        dispatchStarted_ = Clock::now();
        completedBatches_ = 0;
        ticksUntilPoll_ = kLoopCheckInterval;
    }

    // Exactly one small counter and predictable branch on each committed
    // backedge. The 64-bit accounting is updated ONLY once per 128 reductions,
    // never on the hot path. No clock, scheduler, allocation or FFI call here.
    // A no-yield critical section must not increase slow-poll frequency even
    // when overdue; its verified exit may request an explicit cold poll.
    bool completedBackedgeNeedsCheck() noexcept {
        if (--ticksUntilPoll_ != 0) return false;
        ticksUntilPoll_ = kLoopCheckInterval;
        completedBatches_ = detail::saturatingBackedgeBatch(
            completedBatches_, kLoopCheckInterval);
        return true;
    }

    // Cold path: independent watchdog request, reduction cap, or active
    // dispatch elapsed time. This is ONLY advisory: the caller must have a
    // compiler-proven post-commit heap continuation and no live no-yield loans.
    // A watchdog never interrupts or rewinds an executing native stack.
    bool shouldCooperateAtCheck(bool watchdogRequested = false) noexcept {
        if (watchdogRequested || completedBackedgesThisDispatch() >= kMaxBackedgesPerDispatch)
            return true;
        const auto elapsed = detail::elapsedClockTicksOrMax<Clock>(
            Clock::now(), dispatchStarted_);
        const auto threshold = std::chrono::duration_cast<
            typename Clock::duration>(kSoftDispatchQuantum).count();
        return threshold <= 0 || elapsed >= static_cast<std::uint64_t>(threshold);
    }

    // Must only be called for an actual logical cooperate handoff.
    void recordedCooperate() noexcept { lastCooperate_ = Clock::now(); }

    // Generator 'yield value' is NOT a scheduler cooperation.
    std::chrono::nanoseconds timeSinceLastCooperate() const noexcept {
        const auto ticks = detail::elapsedClockTicksOrMax<Clock>(
            Clock::now(), lastCooperate_);
        if (ticks == std::numeric_limits<std::uint64_t>::max())
            return std::chrono::nanoseconds::max();
        // Diagnostics are cold-path only. Convert via wide floating point to
        // avoid duration_cast's signed multiplication overflow for coarse
        // or very large clock tick values, then saturate at max nanoseconds.
        const long double nanos = static_cast<long double>(ticks)
            * static_cast<long double>(Clock::period::num) * 1'000'000'000.0L
            / static_cast<long double>(Clock::period::den);
        if (nanos >= static_cast<long double>(
                std::numeric_limits<std::int64_t>::max()))
            return std::chrono::nanoseconds::max();
        return std::chrono::nanoseconds(static_cast<std::int64_t>(nanos));
    }

    std::uint64_t completedBackedgesThisDispatch() const noexcept {
        return detail::saturatingBackedgeBatch(
            completedBatches_, kLoopCheckInterval - ticksUntilPoll_);
    }

private:
    time_point lastCooperate_;
    time_point dispatchStarted_;
    std::uint64_t completedBatches_ = 0;
    std::uint32_t ticksUntilPoll_ = kLoopCheckInterval;
};

using SourceCooperationQuantum = BasicSourceCooperationQuantum<>;

}  // namespace oreslang::runtime
