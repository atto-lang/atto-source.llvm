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

// Signed chrono subtraction can overflow or appear to produce a short,
// positive-looking duration. Unsigned arithmetic after an ordered comparison
// preserves the mathematical elapsed value and fails closed on regression.
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
        // Await/carrier migration cannot forget an overdue no-yield scope.
        // Only a genuine prior cooperate acknowledges the exhausted counter.
        if (!acknowledgedCooperate_ && backedges_ >= kMaxBackedgesPerDispatch)
            pendingHandoff_ = true;
        acknowledgedCooperate_ = false;
        dispatchStarted_ = Clock::now();
        backedges_ = 0;
        pollDue_ = false;
    }

    // One cheap integer counter/bit-mask on the committed loop backedge.
    // No wall-clock sampling and no scheduler/FFI calls here.
    bool completedBackedgeNeedsCheck() noexcept {
        backedges_ = detail::saturatingBackedgeSuccessor(backedges_);
        // The expensive runtime poll is NEVER invoked on every backedge,
        // even after the fairness budget expires inside a no-yield scope.
        // A newly safe handoff can be delayed by at most 127 backedges;
        // hard untrusted fuel limits require separate charged instrumentation.
        // Poll phase must survive 127-step dispatches and carrier migration.
        pollPhase_ = (pollPhase_ + 1) & (kLoopCheckInterval - 1);
        pollDue_ = pollPhase_ == 0;
        return pollDue_;
    }

    // Cold path: independent watchdog request, reduction cap, or active
    // dispatch elapsed time. This is ONLY advisory: the caller must have a
    // compiler-proven post-commit heap continuation and no live no-yield loans.
    // A watchdog never interrupts or rewinds an executing native stack.
    bool shouldCooperateAtCheck(bool watchdogRequested = false) noexcept {
        // Consume exactly one poll ticket. Calls between true boundaries are
        // cheap and can never repeatedly read the clock.
        if (!pollDue_) return false;
        pollDue_ = false;
        if (pendingHandoff_ || watchdogRequested ||
            backedges_ >= kMaxBackedgesPerDispatch) {
            pendingHandoff_ = true;
            return true;
        }
        const auto elapsed = detail::elapsedClockTicksOrMax<Clock>(
            Clock::now(), dispatchStarted_);
        const auto threshold = std::chrono::duration_cast<
            typename Clock::duration>(kSoftDispatchQuantum).count();
        if (threshold <= 0 || elapsed >= static_cast<std::uint64_t>(threshold)) {
            pendingHandoff_ = true;
            return true;
        }
        return false;
    }

    // Read-only hint for a verified critical/borrow no-yield exit. This
    // never authorizes preemption through a live loan or native stack frame.
    bool pendingHandoffOnNoYieldExit() const noexcept {
        return pendingHandoff_ || backedges_ >= kMaxBackedgesPerDispatch;
    }

    // Must only be called for an actual logical cooperate handoff.
    void recordedCooperate() noexcept {
        lastCooperate_ = Clock::now();
        pendingHandoff_ = false;
        pollDue_ = false;
        acknowledgedCooperate_ = true;
    }

    // Generator 'yield value' is NOT a scheduler cooperation.
    std::chrono::nanoseconds timeSinceLastCooperate() const noexcept {
        const auto ticks = detail::elapsedClockTicksOrMax<Clock>(
            Clock::now(), lastCooperate_);
        if (ticks == std::numeric_limits<std::uint64_t>::max())
            return std::chrono::nanoseconds::max();
        // The diagnostics path is cold only: convert with a wide floating
        // representation so even a coarse custom clock cannot overflow an
        // intermediate signed duration_cast multiplication.
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
    std::uint32_t pollPhase_ = 0;
    bool pollDue_ = false;
    bool pendingHandoff_ = false;
    bool acknowledgedCooperate_ = false;
};

using SourceCooperationQuantum = BasicSourceCooperationQuantum<>;

}  // namespace oreslang::runtime
