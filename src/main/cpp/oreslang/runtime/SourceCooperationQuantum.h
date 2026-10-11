#pragma once

#include <chrono>
#include <cstdint>

namespace oreslang::runtime {

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

    // Calling this after await restarts *only* the fairness quantum.
    void beginDispatch() noexcept {
        dispatchStarted_ = Clock::now();
        backedges_ = 0;
    }

    // One cheap integer counter/bit-mask on the committed loop backedge.
    // No wall-clock sampling and no scheduler/FFI calls here.
    bool completedBackedgeNeedsCheck() noexcept {
        ++backedges_;
        return (backedges_ & (kLoopCheckInterval - 1)) == 0;
    }

    // Cold path: reductions OR active-dispatch monotonic elapsed time.
    bool shouldCooperateAtCheck() noexcept {
        if (backedges_ >= kMaxBackedgesPerDispatch) return true;
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
