#pragma once

#include <cstdint>
#include <mutex>

namespace oreslang::runtime {

/**
 * Trusted host-only, zero-tracing-GC token-bucket primitive.
 *
 * PROTOTYPE ONLY: no native send path calls this yet. It is NOT a security
 * boundary or permission to run untrusted native machine code in-process.
 * Real admission must bind authenticated lineage / supervisor / tenant
 * identities, atomically reserve all levels plus bytes and outstanding
 * mailbox credit, and settle reservation on enqueue/ack/retire.
 *
 * The timestamp must come from the trusted runtime's monotonic clock.
 * Guest code must never receive a reference to this object.
 */
class TrustedOutboundTokenBucket final {
public:
    static constexpr std::uint64_t kSecondNs = 1000000000ULL;
    static constexpr std::uint64_t kMaxRateOrBurst = 1000000000ULL;

    TrustedOutboundTokenBucket(std::uint64_t per_second,
                               std::uint64_t burst,
                               std::uint64_t trusted_initial_nanos) noexcept
        : rate_(per_second), burst_(burst), tokens_(burst),
          last_nanos_(trusted_initial_nanos),
          valid_(per_second > 0 && per_second <= kMaxRateOrBurst
                 && burst > 0 && burst <= kMaxRateOrBurst) {}

    TrustedOutboundTokenBucket(const TrustedOutboundTokenBucket&) = delete;
    TrustedOutboundTokenBucket& operator=(const TrustedOutboundTokenBucket&) = delete;

    bool valid_policy() const noexcept { return valid_; }

    bool try_consume(std::uint64_t count, std::uint64_t trusted_nanos) noexcept {
        if (!valid_ || count == 0 || count > burst_) return false;
        std::lock_guard<std::mutex> lock(mutex_);
        refill(trusted_nanos);
        if (tokens_ < count) return false;
        tokens_ -= count;
        return true;
    }

    // INTERNAL ONLY: reverse a tentative scope charge that was never published.
    // Published reservations permanently spend start-rate tokens; otherwise
    // late guest cancellation could mint capacity after the refill epoch.
    bool rollback_unpublished(std::uint64_t count, std::uint64_t trusted_nanos) noexcept {
        if (!valid_ || count == 0 || count > burst_) return false;
        std::lock_guard<std::mutex> lock(mutex_);
        refill(trusted_nanos);
        tokens_ += count;
        if (tokens_ >= burst_) {
            tokens_ = burst_;
            remainder_ = 0;
        }
        return true;
    }

    std::uint64_t available(std::uint64_t trusted_nanos) noexcept {
        if (!valid_) return 0;
        std::lock_guard<std::mutex> lock(mutex_);
        refill(trusted_nanos);
        return tokens_;
    }

private:
    void refill(std::uint64_t now) noexcept {
        // Monotonic uint64 nanoseconds may wrap. Serial arithmetic allows
        // forward elapsed intervals strictly below 2^63 ns (~292 years),
        // while short backwards/ambiguous samples never mint capacity.
        const std::uint64_t elapsed = now - last_nanos_;
        if (elapsed == 0 || elapsed >= (UINT64_C(1) << 63)) return;
        last_nanos_ = now;
        if (tokens_ == burst_) {
            remainder_ = 0;
            return;
        }
        const std::uint64_t missing = burst_ - tokens_;
        // missing <= 1e9 so numeratorToFull <= 1e18, safely below UINT64_MAX.
        const std::uint64_t numerator_to_full = missing * kSecondNs - remainder_;
        const std::uint64_t ns_to_full = numerator_to_full / rate_
            + (numerator_to_full % rate_ != 0 ? 1 : 0);
        if (elapsed >= ns_to_full) {
            tokens_ = burst_;
            remainder_ = 0;
            return;
        }
        // elapsed * rate_ < 1e18, since elapsed < ns_to_full above.
        const std::uint64_t numerator = elapsed * rate_ + remainder_;
        tokens_ += numerator / kSecondNs;
        remainder_ = numerator % kSecondNs;
    }

    const std::uint64_t rate_;
    const std::uint64_t burst_;
    mutable std::mutex mutex_;
    std::uint64_t tokens_;
    std::uint64_t last_nanos_;
    std::uint64_t remainder_ = 0;
    const bool valid_;
};
} // namespace oreslang::runtime
