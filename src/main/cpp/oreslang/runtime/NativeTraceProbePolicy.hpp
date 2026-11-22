#pragma once

#include <atomic>
#include <cstdint>

namespace oreslang::runtime {

/**
 * Native AOT telemetry gate for probe sites ALREADY emitted by the compiler.
 * Does not JIT, patch code, grant export capabilities or change a guest effect.
 * A sealed/hard-RT image cannot change policy during execution.
 *
 * Internal control-plane owner only; never expose configure() to untrusted
 * source actors. The policy is held per runtime domain, not in a global.
 */
enum class TraceLevel : std::uint8_t {
    Off = 0, Boundaries = 1, Calls = 2, Full = 3
};

class NativeTraceProbePolicy final {
public:
    explicit NativeTraceProbePolicy(TraceLevel compiled, bool sealed = false) noexcept
        : compiled_(compiled), sealed_(sealed) {}

    NativeTraceProbePolicy(const NativeTraceProbePolicy&) = delete;
    NativeTraceProbePolicy& operator=(const NativeTraceProbePolicy&) = delete;

    // One acquire load, no clock, allocation, callback or I/O when off.
    [[nodiscard]] bool should_record(TraceLevel site) const noexcept {
        const auto v = state_.load(std::memory_order_acquire);
        return site != TraceLevel::Off &&
            as_byte(site) <= as_byte(compiled_) &&
            as_byte(site) <= static_cast<std::uint8_t>(v & kModeMask);
    }

    [[nodiscard]] TraceLevel mode() const noexcept {
        const auto v = state_.load(std::memory_order_acquire);
        return static_cast<TraceLevel>(v & kModeMask);
    }

    [[nodiscard]] std::uint64_t generation() const noexcept {
        return state_.load(std::memory_order_acquire) >> kGenerationShift;
    }

    [[nodiscard]] TraceLevel compiled_coverage() const noexcept { return compiled_; }

    // Trusted out-of-band control plane calls this outside the hot path.
    // A mode transition is a single release-published epoch. In-flight spans
    // use captured epoch; collectors fence stale completion at retirement.
    [[nodiscard]] bool reconfigure(TraceLevel next) noexcept {
        if (sealed_ || as_byte(next) > as_byte(compiled_)) return false;
        std::uint64_t old = state_.load(std::memory_order_acquire);
        while (true) {
            if (static_cast<std::uint8_t>(old & kModeMask) == as_byte(next)) return true;
            const auto generation = old >> kGenerationShift;
            if (generation == kMaxGeneration) return false; // fail closed on wrap
            const auto next_state =
                ((generation + 1) << kGenerationShift) | as_byte(next);
            if (state_.compare_exchange_weak(old, next_state,
                                            std::memory_order_acq_rel,
                                            std::memory_order_acquire)) return true;
        }
    }

private:
    static constexpr std::uint64_t kModeMask = 0xffULL;
    static constexpr unsigned kGenerationShift = 8;
    static constexpr std::uint64_t kMaxGeneration =
        UINT64_MAX >> kGenerationShift;

    static constexpr std::uint8_t as_byte(TraceLevel level) noexcept {
        return static_cast<std::uint8_t>(level);
    }

    const TraceLevel compiled_;
    const bool sealed_;
    std::atomic<std::uint64_t> state_{0};
};

} // namespace oreslang::runtime
