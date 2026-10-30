#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace oreslang::runtime {

// Optional, single-writer native carrier-slice trace. Not a timer-to-guest
// dispatch proof: no timer due/notified timestamp is supplied by this carrier.
// The caller owns this object until after carrier.join(); readers must not
// inspect the non-atomic records until the worker has joined.
class CglCarrierDispatchTrace final {
public:
    static constexpr std::size_t capacity = 256;

    struct Sample {
        std::uint32_t actor_slot = 0;
        std::uint64_t actor_generation = 0;
        std::uint64_t message_sequence = 0;
        bool initial_slice = false;
        std::uint64_t slice_start_ns = 0;
        std::uint64_t slice_finish_ns = 0;
    };

    // Single actor-carrier worker only. Fixed-cost, no heap, no lock, no throw.
    bool append(Sample sample) noexcept {
        if (used_ == capacity) {
            if (dropped_ != std::numeric_limits<std::uint64_t>::max()) {
                ++dropped_;
            }
            return false;
        }
        samples_[used_++] = sample;
        return true;
    }

    // Snapshot after join only. A shortened copy does not imply completeness.
    std::size_t copy_after_join(Sample* out, std::size_t out_capacity) const noexcept {
        if (out == nullptr || out_capacity == 0) return 0;
        const auto count = used_ < out_capacity ? used_ : out_capacity;
        for (std::size_t i = 0; i < count; ++i) out[i] = samples_[i];
        return count;
    }

    std::size_t recorded_after_join() const noexcept { return used_; }
    std::uint64_t dropped_after_join() const noexcept { return dropped_; }
    bool complete_after_join() const noexcept { return dropped_ == 0; }

private:
    std::array<Sample, capacity> samples_{};
    std::size_t used_ = 0;
    std::uint64_t dropped_ = 0;
};

} // namespace oreslang::runtime
