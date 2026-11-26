#pragma once

#include "llvm/ADT/APInt.h"
#include "oreslang/types/NativeRefinementProofContract.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string_view>

namespace oreslang {

// Bounds on normal results. LLVM lowering still guards the exceptional paths.
class VerifiedI64Range {
    static constexpr auto Min = std::numeric_limits<std::int64_t>::min();
    static constexpr auto Max = std::numeric_limits<std::int64_t>::max();
    types::NativeI64Interval bounds_;
    unsigned exclusions_ = 0;
    static unsigned bit(std::int64_t n) { return n == 0 ? 1u : n == -1 ? 2u : 0u; }

    static types::NativeI64Interval checked_bounds(std::int64_t low, std::int64_t high) {
        if (low > high) throw std::runtime_error("E_REFINEMENT_EMPTY: contradictory interval");
        return {low, high};
    }

    static llvm::APInt wide(std::int64_t value) {
        return llvm::APInt(128, static_cast<std::uint64_t>(value), true);
    }

    static VerifiedI64Range clip(llvm::APInt first, llvm::APInt last) {
        if (first.slt(wide(Min))) first = wide(Min);
        if (last.sgt(wide(Max))) last = wide(Max);
        return first.sle(last) ? VerifiedI64Range(first.getSExtValue(), last.getSExtValue())
                               : VerifiedI64Range();
    }

public:
    VerifiedI64Range(std::int64_t low = Min, std::int64_t high = Max, unsigned exclusions = 0)
        : bounds_(checked_bounds(low, high)), exclusions_(exclusions) {
        while (bounds_.minimum <= bounds_.maximum && (exclusions_ & bit(bounds_.minimum))) ++bounds_.minimum;
        while (bounds_.minimum <= bounds_.maximum && (exclusions_ & bit(bounds_.maximum))) --bounds_.maximum;
        if (bounds_.minimum > bounds_.maximum) throw std::runtime_error("E_REFINEMENT_EMPTY: contradictory interval");
        if (bounds_.minimum > 0 || bounds_.maximum < 0) exclusions_ &= ~1u;
        if (bounds_.minimum > -1 || bounds_.maximum < -1) exclusions_ &= ~2u;
    }
    bool contains_value(std::int64_t value) const {
        return minimum() <= value && value <= maximum() && !(exclusions_ & bit(value));
    }
    VerifiedI64Range excluding(std::int64_t value) const {
        if (!contains_value(value)) return *this;
        if (minimum() == maximum()) throw std::runtime_error("E_REFINEMENT_EMPTY: contradictory interval");
        if (minimum() == value) return {value+1, maximum(), exclusions_};
        if (maximum() == value) return {minimum(), value-1, exclusions_};
        return {minimum(), maximum(), exclusions_ | bit(value)};
    }
    VerifiedI64Range join(const VerifiedI64Range& other) const {
        unsigned mask = (!contains_value(0) && !other.contains_value(0) ? 1u : 0u)
                      | (!contains_value(-1) && !other.contains_value(-1) ? 2u : 0u);
        return {std::min(minimum(),other.minimum()),std::max(maximum(),other.maximum()),mask};
    }
    bool includes(const VerifiedI64Range& actual) const {
        return actual.bounds_.entails(bounds_) &&
            (!actual.contains_value(0) || contains_value(0)) &&
            (!actual.contains_value(-1) || contains_value(-1));
    }

    std::int64_t minimum() const { return bounds_.minimum; }
    std::int64_t maximum() const { return bounds_.maximum; }

    bool nonnegative() const { return bounds_.minimum >= 0; }
    bool positive() const { return bounds_.minimum > 0; }
    bool nonpositive() const { return bounds_.maximum <= 0; }
    bool at_most(const VerifiedI64Range& right) const { return bounds_.maximum <= right.bounds_.minimum; }

    static VerifiedI64Range signed_i32() {
        return {std::numeric_limits<std::int32_t>::min(), std::numeric_limits<std::int32_t>::max()};
    }

    VerifiedI64Range on_normal_result_within(const VerifiedI64Range& carrier) const {
        const auto low = std::max(bounds_.minimum, carrier.bounds_.minimum);
        const auto high = std::min(bounds_.maximum, carrier.bounds_.maximum);
        return low <= high ? VerifiedI64Range(low, high) : carrier;
    }

    VerifiedI64Range constrain(std::string_view op, std::int64_t bound) const {
        if (op == "eq") return {std::max(bounds_.minimum, bound), std::min(bounds_.maximum, bound), exclusions_};
        if (op == "<=") return {bounds_.minimum, std::min(bounds_.maximum, bound)};
        if (op == ">=") return {std::max(bounds_.minimum, bound), bounds_.maximum, exclusions_};
        if (op == "<" && bound != Min) return {bounds_.minimum, std::min(bounds_.maximum, bound - 1), exclusions_};
        if (op == ">" && bound != Max) return {std::max(bounds_.minimum, bound + 1), bounds_.maximum, exclusions_};
        if (op == "<" || op == ">")
            throw std::runtime_error("E_REFINEMENT_EMPTY: contradictory interval");
        throw std::runtime_error("E_REFINEMENT_PREDICATE: expected an interval comparison");
    }

    VerifiedI64Range add(const VerifiedI64Range& right) const {
        return clip(wide(bounds_.minimum) + wide(right.bounds_.minimum), wide(bounds_.maximum) + wide(right.bounds_.maximum));
    }

    VerifiedI64Range subtract(const VerifiedI64Range& right) const {
        return clip(wide(bounds_.minimum) - wide(right.bounds_.maximum), wide(bounds_.maximum) - wide(right.bounds_.minimum));
    }

    VerifiedI64Range multiply(const VerifiedI64Range& right) const {
        std::array values{
            wide(bounds_.minimum) * wide(right.bounds_.minimum), wide(bounds_.minimum) * wide(right.bounds_.maximum),
            wide(bounds_.maximum) * wide(right.bounds_.minimum), wide(bounds_.maximum) * wide(right.bounds_.maximum)
        };
        auto bounds = std::minmax_element(values.begin(), values.end(),
            [](const auto& lhs, const auto& rhs) { return lhs.slt(rhs); });
        return clip(*bounds.first, *bounds.second);
    }

    VerifiedI64Range divide(const VerifiedI64Range& right) const {
        if ((right.bounds_.minimum <= 0 && right.bounds_.maximum >= 0)
                || (bounds_.minimum == Min && right.bounds_.minimum <= -1 && right.bounds_.maximum >= -1))
            return {};
        const auto bounds = std::minmax({bounds_.minimum / right.bounds_.minimum, bounds_.minimum / right.bounds_.maximum,
            bounds_.maximum / right.bounds_.minimum, bounds_.maximum / right.bounds_.maximum});
        return {bounds.first, bounds.second};
    }
};

} // namespace oreslang
