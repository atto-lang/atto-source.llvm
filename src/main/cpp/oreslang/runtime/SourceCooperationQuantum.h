#pragma once

#include <chrono>
#include <cstdint>
#include <limits>

namespace oreslang::runtime {

// Saturation preserves the mandatory reduction cap even after an extremely
// prolonged no-yield critical section. Kept constexpr for boundary testing.
namespace detail {
constexpr std::uint64_t saturatingBackedgeSuccessor(
        std::uint64_t completed) noexcept {
    return completed == std::numeric_limits<std::uint64_t>::max()
        ? completed : completed + 1;
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
    static constexpr std::uint32_t kLoopCheckInterval = 64;
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
    }

    // One cheap integer counter/bit-mask on the committed loop backedge.
    // No wall-clock sampling and no scheduler/FFI calls here.
    bool completedBackedgeNeedsCheck() noexcept {
        backedges_ = detail::saturatingBackedgeSuccessor(backedges_);
        // After an over-budget no-yield region, do not defer a newly
        // available safe handoff for another 64 backedges. The caller still
        // decides whether a protected lexical scope forbids suspension.
        return backedges_ >= kMaxBackedgesPerDispatch ||
            (backedges_ & (kLoopCheckInterval - 1)) == 0;
    }

    // Cold path: independent watchdog request, reduction cap, or active
    // dispatch elapsed time. This is ONLY advisory: the caller must have a
    // compiler-proven post-commit heap continuation and no live no-yield loans.
    // A watchdog never interrupts or rewinds an executing native stack.
    bool shouldCooperateAtCheck(bool watchdogRequested = false) noexcept {
        if (watchdogRequested || backedges_ >= kMaxBackedgesPerDispatch)
            return true;
        return Clock::now() - dispatchStarted_ >= kSoftDispatchQuantum;
    }

    // Must only be called for an actual logical cooperate handoff.
    void recordedCooperate() noexcept { lastCooperate_ = Clock::now(); }

    // Generator 'yield value' is NOT a scheduler cooperation.
    std::chrono::nanoseconds timeSinceLastCooperate() const noexcept {
        const auto elapsed = Clock::now() - lastCooperate_;
        const auto nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed);
        return nanos.count() < 0 ? std::chrono::nanoseconds::zero() : nanos;
    }

    std::uint64_t completedBackedgesThisDispatch() const noexcept { return backedges_; }

private:
    time_point lastCooperate_;
    time_point dispatchStarted_;
    std::uint64_t backedges_ = 0;
};

using SourceCooperationQuantum = BasicSourceCooperationQuantum<>;

}  // namespace oreslang::runtime
