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
    unsigned holes_ = 0;
    static unsigned bit(std::int64_t n) { return n==0?1u:n==-1?2u:0u; }
    VerifiedI64Range(std::int64_t lo,std::int64_t hi,unsigned holes)
        : bounds_(checked_bounds(lo,hi)), holes_(holes) {
        while(holes_&bit(lo))++lo;
        while(holes_&bit(hi))--hi;
        bounds_=checked_bounds(lo,hi);
    }

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
        return actual.bounds_.entails(bounds_) &&
            (!actual.has(0)||has(0)) && (!actual.has(-1)||has(-1));
    }

    bool has(std::int64_t n) const {
        return minimum()<=n&&n<=maximum()&&!(holes_&bit(n));
    }
    VerifiedI64Range join(const VerifiedI64Range& b) const {
        return VerifiedI64Range(std::min(minimum(),b.minimum()),std::max(maximum(),b.maximum()),
            (!has(0)&&!b.has(0)?1u:0u)|(!has(-1)&&!b.has(-1)?2u:0u));
    }
    VerifiedI64Range restrict(std::string_view op,std::int64_t n) const {
        try {
            if(op=="!=") {
                if(!has(n))return *this;
                if(n==minimum())return n==Max?*this:VerifiedI64Range(n+1,maximum(),holes_);
                if(n==maximum())return n==Min?*this:VerifiedI64Range(minimum(),n-1,holes_);
                return VerifiedI64Range(minimum(),maximum(),holes_|bit(n));
            }
            auto r=constrain(op=="=="?"eq":op,n);
            return VerifiedI64Range(r.minimum(),r.maximum(),holes_);
        } catch(const std::runtime_error&) { return *this; }
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
        if (op == "/") return !right.has(0) && !(has(Min)&&right.has(-1));
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
