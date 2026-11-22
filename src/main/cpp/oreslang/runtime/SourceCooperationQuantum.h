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
        backedges_ = 0;
        checkPhase_ = 0;
        checkDue_ = false;
    }

    // Cheap reduction accounting only: no clock, callback or scheduler read.
    // The separate phase wraps independently of the saturating total, so even
    // an astronomically long no-yield scope cannot disable future polls.
    // The slow path is eligible only once per 128 committed backedges,
    // including AFTER exceeding the 512-backedge dispatch budget.
    bool completedBackedgeNeedsCheck() noexcept {
        backedges_ = detail::saturatingBackedgeSuccessor(backedges_);
        checkPhase_ = (checkPhase_ + 1) & (kLoopCheckInterval - 1);
        checkDue_ = (checkPhase_ == 0);
        return checkDue_;
    }

    // Cold path: independent watchdog request, reduction cap, or active
    // dispatch elapsed time. This is ONLY advisory: the caller must have a
    // compiler-proven post-commit heap continuation and no live no-yield loans.
    // A watchdog never interrupts or rewinds an executing native stack.
    bool shouldCooperateAtCheck(bool watchdogRequested = false) noexcept {
        // An accidental eager caller must not bypass batching (even when a
        // supervisor has requested cancellation) or sample the clock on
        // every backedge. Consume the boundary ticket exactly once.
        if (!checkDue_)
            return false;
        checkDue_ = false;
        if (watchdogRequested || backedges_ >= kMaxBackedgesPerDispatch)
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

    std::uint64_t completedBackedgesThisDispatch() const noexcept { return backedges_; }

private:
    time_point lastCooperate_;
    time_point dispatchStarted_;
    std::uint64_t backedges_ = 0;
    std::uint32_t checkPhase_ = 0;
    bool checkDue_ = false;
};

using SourceCooperationQuantum = BasicSourceCooperationQuantum<>;

}  // namespace oreslang::runtime
