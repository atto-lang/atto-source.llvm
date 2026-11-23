#pragma once

#include <array>
#include <cstdint>
#include <limits>
#include <stdexcept>

namespace oreslang::dop {

// An allocation-free, fixed-capacity AOT observation buffer. The owner must
// enforce actor/turn isolation; no mutable state may cross concurrent turns.
// Observations are advisory only: they never allocate, migrate storage, JIT,
// patch code, grant guest authority, or expose raw addresses.
enum class LayoutTraceHint : std::uint8_t { Insufficient, AoS, SoA };

class DopAotLayoutTrace final {
public:
    static constexpr std::uint32_t max_fields = 64;

    // A sealed trace has a compile-time/configuration-frozen enable mode.
    // Even an enabled trace is bounded, clockless and free of host callbacks.
    explicit DopAotLayoutTrace(std::uint64_t rows, std::uint32_t fields,
                               bool enabled = false, bool sealed = false)
        : rows_(rows), fields_(fields), enabled_(enabled), sealed_(sealed) {
        if (fields == 0 || fields > max_fields)
            throw std::invalid_argument("AOT layout trace requires 1..64 fields");
    }

    DopAotLayoutTrace(const DopAotLayoutTrace&) = delete;
    DopAotLayoutTrace& operator=(const DopAotLayoutTrace&) = delete;

    // Only a trusted owner may toggle an unsealed trace. Caller must hold
    // exclusive execution authority; this class intentionally has no locks.
    [[nodiscard]] bool configure(bool enabled) noexcept {
        if (sealed_) return false;
        enabled_ = enabled;
        clear_history();
        return true;
    }

    [[nodiscard]] bool enabled() const noexcept { return enabled_; }
    [[nodiscard]] bool sealed() const noexcept { return sealed_; }
    [[nodiscard]] std::uint32_t fields() const noexcept { return fields_; }

    // An invalid row/field or disabled probe changes nothing and returns false.
    // Repeated accesses saturate instead of wrapping 64-bit sample counters.
    [[nodiscard]] bool observe(std::uint64_t row, std::uint32_t field) noexcept {
        if (!enabled_ || row >= rows_ || field >= fields_) return false;
        increment(samples_);
        increment(field_hits_[field]);
        if (have_previous_) {
            if (row == last_row_ && field != last_field_)
                increment(row_local_transitions_);
            else if (field == last_field_ && last_row_ != UINT64_MAX
                        && row == last_row_ + 1)
                increment(column_scan_transitions_);
        }
        last_row_ = row;
        last_field_ = field;
        have_previous_ = true;
        return true;
    }

    // A hint needs >=2 locality transitions; ties or random probes remain
    // unclassified. This is NOT a cache/SIMD benchmark or a migration order.
    [[nodiscard]] LayoutTraceHint hint() const noexcept {
        if (column_scan_transitions_ >= 2 &&
            column_scan_transitions_ > row_local_transitions_)
            return LayoutTraceHint::SoA;
        if (row_local_transitions_ >= 2 &&
            row_local_transitions_ > column_scan_transitions_)
            return LayoutTraceHint::AoS;
        return LayoutTraceHint::Insufficient;
    }

    [[nodiscard]] std::uint64_t samples() const noexcept { return samples_; }
    [[nodiscard]] std::uint64_t field_hits(std::uint32_t field) const {
        if (field >= fields_) throw std::out_of_range("layout trace field out of range");
        return field_hits_[field];
    }
    [[nodiscard]] std::uint64_t row_local_transitions() const noexcept {
        return row_local_transitions_;
    }
    [[nodiscard]] std::uint64_t column_scan_transitions() const noexcept {
        return column_scan_transitions_;
    }
    void reset() noexcept {
        samples_ = 0;
        field_hits_.fill(0);
        row_local_transitions_ = 0;
        column_scan_transitions_ = 0;
        clear_history();
    }

private:
    static void increment(std::uint64_t& value) noexcept {
        if (value != std::numeric_limits<std::uint64_t>::max()) ++value;
    }
    void clear_history() noexcept {
        have_previous_ = false;
        last_row_ = 0;
        last_field_ = 0;
    }

    const std::uint64_t rows_;
    const std::uint32_t fields_;
    bool enabled_;
    const bool sealed_;
    std::array<std::uint64_t, max_fields> field_hits_{};
    std::uint64_t samples_ = 0;
    std::uint64_t row_local_transitions_ = 0;
    std::uint64_t column_scan_transitions_ = 0;
    std::uint64_t last_row_ = 0;
    std::uint32_t last_field_ = 0;
    bool have_previous_ = false;
};

} // namespace oreslang::dop
