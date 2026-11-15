#pragma once

#include <chrono>
#include <cstdint>
#include <limits>
#include <stdexcept>

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
    static constexpr std::uint32_t kLoopCheckInterval = 128;
    static constexpr std::uint32_t kMaxBackedgesPerDispatch = 512;
    static constexpr auto kSoftDispatchQuantum = std::chrono::milliseconds(2);

    // Default 128-backedge soft-poll boundary. A compiler-verified 256/512
    // option batches ordinary soft polls further; the hard 512-backedge
    // no-yield handoff rule is NEVER diluted by an option.
    explicit BasicSourceCooperationQuantum(std::uint32_t interval = kLoopCheckInterval)
        : lastCooperate_(Clock::now()), dispatchStarted_(lastCooperate_),
          loopCheckInterval_(interval) {
        if (interval != 128 && interval != 256 && interval != 512)
            throw std::invalid_argument("unsupported cooperation interval");
    }

    // A budget belongs to one logical task: copying or moving would split
    // its reduction history from the saved heap continuation. Carrier
    // migration transports the owning task, never a copied quantum.
    BasicSourceCooperationQuantum(const BasicSourceCooperationQuantum&) = delete;
    BasicSourceCooperationQuantum& operator=(const BasicSourceCooperationQuantum&) = delete;
    BasicSourceCooperationQuantum(BasicSourceCooperationQuantum&&) = delete;
    BasicSourceCooperationQuantum& operator=(BasicSourceCooperationQuantum&&) = delete;

    // Calling this after await restarts *only* the per-dispatch fairness
    // counter/clock and invalidates any stale boundary ticket. A watchdog or
    // overdue cooperation request survives suspension and carrier migration
    // until an actual legal handoff calls recordedCooperate().
    void beginDispatch() noexcept {
        // A caller may resume a task after its 512-backedge cap while the
        // no-yield scope was still live. Latch debt before resetting the
        // transient counter; resetting must never forgive overdue work.
        pendingHandoff_ = pendingHandoff_ ||
            backedges_ >= kMaxBackedgesPerDispatch;
        dispatchStarted_ = Clock::now();
        backedges_ = 0;
        checkPhase_ = 0;
        checkDue_ = false;
    }

    // Cheap reduction accounting only: no clock, callback or scheduler read.
    // The separate phase wraps independently of the saturating total, so even
    // an astronomically long no-yield scope cannot disable future polls.
    // The slow path is eligible only once per configured interval of
    // committed backedges, including AFTER exceeding the 512-backedge budget.
    bool completedBackedgeNeedsCheck() noexcept {
        backedges_ = detail::saturatingBackedgeSuccessor(backedges_);
        checkPhase_ = (checkPhase_ + 1) & (loopCheckInterval_ - 1);
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
        // A prior overdue signal stays pending across an unyieldable region.
        // Avoid repeated clock reads even at later batched poll boundaries.
        if (pendingHandoff_ || watchdogRequested ||
                backedges_ >= kMaxBackedgesPerDispatch) {
            pendingHandoff_ = true;
            return true;
        }
        if (Clock::now() - dispatchStarted_ >= kSoftDispatchQuantum) {
            pendingHandoff_ = true;
            return true;
        }
        return false;
    }

    // The compiler MUST call this once on every verified exit of a no-yield
    // critical/borrow scope. This is NOT a scheduler call or a clock read;
    // it only reports a sticky request. Suspension remains forbidden until
    // all live loans/guards are released and the heap frame is resumable.
    bool pendingHandoffOnNoYieldExit() const noexcept {
        return pendingHandoff_ ||
            backedges_ >= kMaxBackedgesPerDispatch;
    }

    // Only a genuine legal scheduler handoff acknowledges pending debt.
    // Reset the exhausted dispatch counter as part of that acknowledgement:
    // otherwise the next beginDispatch() would relatch the already-serviced
    // 512-cap and an exit hook could spuriously request another handoff.
    // This must never be called for a plain await, generator yield, or a
    // still-held no-yield/ownership guard.
    void recordedCooperate() noexcept {
        lastCooperate_ = Clock::now();
        pendingHandoff_ = false;
        backedges_ = 0;
    }

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
    std::uint32_t loopCheckInterval_;
    std::uint32_t checkPhase_ = 0;
    bool checkDue_ = false;
    bool pendingHandoff_ = false;
};

using SourceCooperationQuantum = BasicSourceCooperationQuantum<>;

}  // namespace oreslang::runtime
