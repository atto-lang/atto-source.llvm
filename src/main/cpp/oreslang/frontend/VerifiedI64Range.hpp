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
    VerifiedI64Range(std::int64_t low = Min, std::int64_t high = Max) : bounds_(checked_bounds(low, high)) {}

    bool includes(const VerifiedI64Range& actual) const {
        return actual.bounds_.entails(bounds_);
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
        if (op == "eq") return {std::max(bounds_.minimum, bound), std::min(bounds_.maximum, bound)};
        if (op == "<=") return {bounds_.minimum, std::min(bounds_.maximum, bound)};
        if (op == ">=") return {std::max(bounds_.minimum, bound), bounds_.maximum};
        if (op == "<" && bound != Min) return {bounds_.minimum, std::min(bounds_.maximum, bound - 1)};
        if (op == ">" && bound != Max) return {std::max(bounds_.minimum, bound + 1), bounds_.maximum};
        if (op == "<" || op == ">")
            throw std::runtime_error("E_REFINEMENT_EMPTY: contradictory interval");
        throw std::runtime_error("E_REFINEMENT_PREDICATE: expected an interval comparison");
    }

    // Universal safety, never inferred from clipped normal-result bounds.
    bool proves_arithmetic(std::string_view op, const VerifiedI64Range& right) const {
        auto fits = [](const llvm::APInt& low, const llvm::APInt& high) {
            return !low.slt(wide(Min)) && !high.sgt(wide(Max));
        };
        auto a=wide(minimum()), b=wide(maximum()), c=wide(right.minimum()), d=wide(right.maximum());
        if (op == "+") return fits(a+c,b+d);
        if (op == "-") return fits(a-d,b-c);
        if (op == "*") {
            std::array values{a*c,a*d,b*c,b*d};
            auto ends=std::minmax_element(values.begin(),values.end(),
                [](const auto& l,const auto& r){return l.slt(r);});
            return fits(*ends.first,*ends.second);
        }
        if (op == "/") return (right.minimum()>0 || right.maximum()<0)
            && !(minimum()==Min && right.minimum()<=-1 && right.maximum()>=-1);
        return false;
    }
    bool proves_negation() const { return minimum()!=Min; }

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
